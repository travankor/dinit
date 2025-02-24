#ifndef SERVICE_H
#define SERVICE_H

#include <string>
#include <list>
#include <vector>
#include <csignal>
#include <unordered_set>
#include <algorithm>

#include "dasynq.h"

#include "dinit.h"
#include "control.h"
#include "service-listener.h"
#include "service-constants.h"
#include "load-service.h"
#include "dinit-ll.h"
#include "dinit-log.h"

/*
 * This header defines service_record, a data record maintaining information about a service,
 * and service_set, a set of interdependent service records. It also defines some associated
 * types and exceptions.
 *
 * Service states
 * --------------
 * Services have both a current state and a desired state. The desired state can be
 * either STARTED or STOPPED. The current state can also be STARTING or STOPPING.
 * A service can be "pinned" in either the STARTED or STOPPED states to prevent it
 * from leaving that state until it is unpinned.
 *
 * The total state is a combination of the two, current and desired:
 *      STOPPED/STOPPED  : stopped and will remain stopped
 *      STOPPED/STARTED  : stopped (pinned), must be unpinned to start
 *      STARTING/STARTED : starting, but not yet started. Dependencies may also be starting.
 *      STARTING/STOPPED : as above, but the service will be stopped again as soon as it has
 *                         completed startup.
 *      STARTED/STARTED  : running and will continue running.
 *      STARTED/STOPPED  : started (pinned), must be unpinned to stop
 *      STOPPING/STOPPED : stopping and will stop. Dependents may be stopping.
 *      STOPPING/STARTED : as above, but the service will be re-started again once it stops.
 *
 * A scripted service is in the STARTING/STOPPING states during the script execution.
 * A process service is in the STOPPING state when it has been signalled to stop, and is
 * in the STARTING state when waiting for dependencies to start or for the exec() call in
 * the forked child to complete and return a status.
 *
 * Acquisition/release:
 * ------------------
 * Each service has a dependent-count ("required_by"). This starts at 0, adds 1 if the
 * service has explicitly been started (i.e. "start_explicit" is true), and adds 1 for
 * each dependent service which is not STOPPED (including dependents with a soft dependency).
 * When required_by transitions to 0, the service is stopped (unless it is pinned). When
 * require_by transitions from 0, the service is started (unless pinned).
 *
 * So, in general, the dependent-count determines the desired state (STARTED if the count
 * is greater than 0, otherwise STOPPED). However, a service can be issued a stop-and-take
 * down order (via `stop(true)'); this will first stop dependent services, which may restart
 * and cancel the stop of the former service. Finally, a service can be force-stopped, which
 * means that its stop process cannot be cancelled (though it may still be put in a desired
 * state of STARTED, meaning it will start immediately upon stopping).
 *
 * Pinning
 * -------
 * A service may be "pinned" in either STARTED or STOPPED states (or even both). Once it
 * reaches a pinned state, a service will not leave that state, though its desired state
 * may still be set. (Note that pinning prevents, but never causes, state transition).
 *
 * The priority of the different state deciders is:
 *  - pins
 *  - force stop flag
 *  - desired state (which is manipulated by require/release operations)
 *
 * So a forced stop cannot occur until the service is not pinned started, for instance.
 *
 * Two-phase transition
 * --------------------
 * Transition between states occurs in two phases: propagation and execution. In both phases
 * a linked-list queue is used to keep track of which services need processing; this avoids
 * recursion (which would be of unknown depth and therefore liable to stack overflow).
 *
 * In the propagation phase, acquisition/release messages are processed, and desired state may be
 * altered accordingly. Start and stop requests are also propagated in this phase. The state may
 * be set to STARTING or STOPPING to reflect the desired state, but will never be set to STARTED
 * or STOPPED (that happens in the execution phase).
 *
 * The two-phase transition is needed to avoid problem where a service that becomes STOPPED has
 * an incorrect acquisition count, which may cause it to restart when it should not. The
 * propagation phase allows the acquisition count to settle before the transition to the STOPPED
 * state occurs, and the decision whether to restart can then be made based on the (correct)
 * acquisition count.
 *
 * Propagation variables:
 *   prop_acquire:  the service has transitioned to an acquired state and must issue an acquire
 *                  on its dependencies
 *   prop_release:  the service has transitioned to a released state and must issue a release on
 *                  its dependencies.
 *
 *   prop_start:    the service should start
 *   prop_stop:     the service should stop
 *
 * Note that "prop_acquire"/"prop_release" form a pair which cannot both be set at the same time
 * which is enforced via explicit checks. For "prop_start"/"prop_stop" this occurs implicitly.
 *
 * In the execution phase, actions are taken to achieve the desired state. Actual state may
 * transition according to the current and desired states. Processes can be sent signals, etc
 * in order to stop them. A process can restart if it stops, but it does so by raising prop_start
 * which needs to be processed in a second transition phase. Seeing as starting never causes
 * another process to stop, the transition-execute-transition cycle always ends at the 2nd
 * transition stage, at the latest.
 */

struct service_flags_t
{
    // on-start flags:
    bool rw_ready : 1;  // file system should be writable once this service starts
    bool log_ready : 1; // syslog should be available once this service starts
    
    // Other service options flags:
    bool no_sigterm : 1;  // do not send SIGTERM
    bool runs_on_console : 1;  // run "in the foreground"
    bool starts_on_console : 1; // starts in the foreground
    bool shares_console : 1;    // run on console, but not exclusively
    bool pass_cs_fd : 1;  // pass this service a control socket connection via fd
    bool start_interruptible : 1; // the startup of this service process is ok to interrupt with SIGINT
    bool skippable : 1;   // if interrupted the service is skipped (scripted services)
    bool signal_process_only : 1;  // signal the session process, not the whole group
    
    service_flags_t() noexcept : rw_ready(false), log_ready(false), no_sigterm(false),
            runs_on_console(false), starts_on_console(false), shares_console(false),
            pass_cs_fd(false), start_interruptible(false), skippable(false), signal_process_only(false)
    {
    }
};

class service_record;
class service_set;
class base_process_service;

/* Service dependency record */
class service_dep
{
    service_record * from;
    service_record * to;

    public:
    /* Whether the 'from' service is waiting for the 'to' service to start */
    bool waiting_on;
    /* Whether the 'from' service is holding an acquire on the 'to' service */
    bool holding_acq;

    const dependency_type dep_type;

    // Check if the dependency is a hard dependency (including milestone still waiting).
    bool is_hard()
    {
        return dep_type == dependency_type::REGULAR
                || (dep_type == dependency_type::MILESTONE && waiting_on);
    }

    service_dep(service_record * from, service_record * to, dependency_type dep_type_p) noexcept
            : from(from), to(to), waiting_on(false), holding_acq(false), dep_type(dep_type_p)
    {  }

    service_dep(const service_dep &) = delete;
    void operator=(const service_dep &) = delete;

    service_record * get_from() const noexcept
    {
        return from;
    }

    service_record * get_to() const noexcept
    {
        return to;
    }
};

/* preliminary service dependency information */
class prelim_dep
{
    public:
    service_record * const to;
    dependency_type const dep_type;

    prelim_dep(service_record *to_p, dependency_type dep_type_p) : to(to_p), dep_type(dep_type_p)
    {
        //
    }
};

// service_record: base class for service record containing static information
// and current state of each service.
//
// This abstract base class defines the dependency behaviour of services. The actions to actually bring a
// service up or down are specified by subclasses in the virtual methods (see especially bring_up() and
// bring_down()).
//
class service_record
{
    protected:
    using string = std::string;
    using time_val = dasynq::time_val;
    
    private:
    string service_name;
    service_type_t record_type;  // service_type_t::DUMMY, PROCESS, SCRIPTED, or INTERNAL

    // 'service_state' can be any valid state: STARTED, STARTING, STOPPING, STOPPED.
    // 'desired_state' is only set to final states: STARTED or STOPPED.
    service_state_t service_state = service_state_t::STOPPED;
    service_state_t desired_state = service_state_t::STOPPED;

    protected:
    string pid_file;
    
    service_flags_t onstart_flags;

    string logfile;           // log file name, empty string specifies /dev/null
    
    bool auto_restart : 1;    // whether to restart this (process) if it dies unexpectedly
    bool smooth_recovery : 1; // whether the service process can restart without bringing down service
    
    bool pinned_stopped : 1;
    bool pinned_started : 1;
    bool waiting_for_deps : 1;  // if STARTING, whether we are waiting for dependencies/console
                                // if STOPPING, whether we are waiting for dependents to stop
    bool waiting_for_console : 1;   // waiting for exclusive console access (while STARTING)
    bool have_console : 1;      // whether we have exclusive console access (STARTING/STARTED)
    bool waiting_for_execstat : 1;  // if we are waiting for exec status after fork()
    bool start_explicit : 1;    // whether we are are explicitly required to be started

    bool prop_require : 1;      // require must be propagated
    bool prop_release : 1;      // release must be propagated
    bool prop_failure : 1;      // failure to start must be propagated
    bool prop_start   : 1;
    bool prop_stop    : 1;

    bool restarting   : 1;      // re-start after stopping
    bool start_failed : 1;      // failed to start (reset when begins starting)
    bool start_skipped : 1;     // start was skipped by interrupt
    
    int required_by = 0;        // number of dependents wanting this service to be started

    // list of dependencies
    typedef std::list<service_dep> dep_list;
    
    // list of dependents
    typedef std::list<service_dep *> dpt_list;
    
    dep_list depends_on;  // services this one depends on
    dpt_list dependents;  // services depending on this one
    
    service_set *services; // the set this service belongs to
    
    std::unordered_set<service_listener *> listeners;
    
    // Process services:
    bool force_stop; // true if the service must actually stop. This is the
                     // case if for example the process dies; the service,
                     // and all its dependencies, MUST be stopped.
    
    int term_signal = -1;  // signal to use for process termination
    
    string socket_path; // path to the socket for socket-activation service
    int socket_perms;   // socket permissions ("mode")
    uid_t socket_uid = -1;  // socket user id or -1
    gid_t socket_gid = -1;  // socket group id or -1

    stopped_reason_t stop_reason = stopped_reason_t::NORMAL;  // reason why stopped

    string start_on_completion;  // service to start when this one completes

    // Data for use by service_set
    public:
    
    // Console queue.
    lld_node<service_record> console_queue_node;
    
    // Propagation and start/stop queues
    lls_node<service_record> prop_queue_node;
    lls_node<service_record> stop_queue_node;
    
    protected:

    // Service has actually stopped (includes having all dependents
    // reaching STOPPED state).
    void stopped() noexcept;
    
    // Service has successfully started
    void started() noexcept;
    
    // Service failed to start (only called when in STARTING state).
    //   dep_failed: whether failure is recorded due to a dependency failing
    //   immediate_stop: whether to set state as STOPPED and handle complete stop.
    void failed_to_start(bool dep_failed = false, bool immediate_stop = true) noexcept;
    
    // A dependency has reached STARTED state
    void dependency_started() noexcept;
    
    void all_deps_started() noexcept;

    // Start all dependencies, return true if all have started
    bool start_check_dependencies() noexcept;

    // Check whether all dependencies have started (i.e. whether we can start now)
    bool check_deps_started() noexcept;

    // Whether a STOPPING service can immediately transition to STARTED.
    bool can_interrupt_stop() noexcept
    {
        return waiting_for_deps && ! force_stop;
    }

    // A dependent has reached STOPPED state
    void dependent_stopped() noexcept;

    // check if all dependents have stopped
    bool stop_check_dependents() noexcept;
    
    // issue a stop to all dependents, return true if they are all already stopped
    bool stop_dependents() noexcept;
    
    void require() noexcept;
    void release(bool issue_stop = true) noexcept;
    void release_dependencies() noexcept;
    
    // Check if service is, fundamentally, stopped.
    bool is_stopped() noexcept
    {
        return service_state == service_state_t::STOPPED
            || (service_state == service_state_t::STARTING && waiting_for_deps);
    }
    
    void notify_listeners(service_event_t event) noexcept
    {
        for (auto l : listeners) {
            l->service_event(this, event);
        }
    }
    
    // Queue to run on the console. 'acquired_console()' will be called when the console is available.
    // Has no effect if the service has already queued for console.
    void queue_for_console() noexcept;
    
    // Release console (console must be currently held by this service)
    void release_console() noexcept;
    
    // Started state reached
    bool process_started() noexcept;

    // Called on transition of desired state from stopped to started (or unpinned stop)
    void do_start() noexcept;

    // Begin stopping, release activation.
    void do_stop() noexcept;

    // Set the service state
    void set_state(service_state_t new_state) noexcept
    {
        service_state = new_state;
    }

    // Virtual functions, to be implemented by service implementations:

    // Do any post-dependency startup; return false on failure
    virtual bool bring_up() noexcept;

    // All dependents have stopped, and this service should proceed to stop.
    virtual void bring_down() noexcept;

    // Whether a STARTING service can immediately transition to STOPPED (as opposed to
    // having to wait for it reach STARTED and then go through STOPPING).
    virtual bool can_interrupt_start() noexcept
    {
        return waiting_for_deps;
    }

    // Whether a STARTING service can transition to its STARTED state, once all
    // dependencies have started.
    virtual bool can_proceed_to_start() noexcept
    {
        return true;
    }

    // Interrupt startup. Returns true if service start is fully cancelled; returns false if cancel order
    // issued but service has not yet responded (state will be set to STOPPING).
    virtual bool interrupt_start() noexcept;

    // The service is becoming inactive - i.e. it has stopped and will not be immediately restarted. Perform
    // any appropriate cleanup.
    virtual void becoming_inactive() noexcept { }

    public:

    service_record(service_set *set, const string &name)
        : service_name(name), service_state(service_state_t::STOPPED),
            desired_state(service_state_t::STOPPED), auto_restart(false), smooth_recovery(false),
            pinned_stopped(false), pinned_started(false), waiting_for_deps(false),
            waiting_for_console(false), have_console(false), waiting_for_execstat(false),
            start_explicit(false), prop_require(false), prop_release(false), prop_failure(false),
            prop_start(false), prop_stop(false), restarting(false), start_failed(false),
            start_skipped(false), force_stop(false)
    {
        services = set;
        record_type = service_type_t::DUMMY;
        socket_perms = 0;
    }

    service_record(service_set *set, const string &name, service_type_t record_type_p,
            const std::list<prelim_dep> &deplist_p)
        : service_record(set, name)
    {
        services = set;
        service_name = name;
        this->record_type = record_type_p;

        try {
            for (auto & pdep : deplist_p) {
                auto b = depends_on.emplace(depends_on.end(), this, pdep.to, pdep.dep_type);
                try {
                    pdep.to->dependents.push_back(&(*b));
                }
                catch (...) {
                    // we'll roll back one now and re-throw:
                    depends_on.pop_back();
                    throw;
                }
            }
        }
        catch (...) {
            for (auto & dep : depends_on) {
                dep.get_to()->dependents.pop_back();
            }
            throw;
        }
    }

    service_record(const service_record &) = delete;
    void operator=(const service_record &) = delete;

    virtual ~service_record() noexcept
    {
    }
    
    // Get the type of this service record
    service_type_t get_type() noexcept
    {
        return record_type;
    }

    // begin transition from stopped to started state or vice versa depending on current and desired state
    void execute_transition() noexcept;
    
    void do_propagation() noexcept;

    // Console is available.
    void acquired_console() noexcept;
    
    // Get the target (aka desired) state.
    service_state_t get_target_state() noexcept
    {
        return desired_state;
    }

    // Is the service explicitly marked active?
    bool is_marked_active() noexcept
    {
        return start_explicit;
    }

    // Set logfile, should be done before service is started
    void set_log_file(const string &logfile)
    {
        this->logfile = logfile;
    }
    
    // Set whether this service should automatically restart when it dies
    void set_auto_restart(bool auto_restart) noexcept
    {
        this->auto_restart = auto_restart;
    }
    
    void set_smooth_recovery(bool smooth_recovery) noexcept
    {
        this->smooth_recovery = smooth_recovery;
    }
    
    // Set "on start" flags (commands)
    void set_flags(service_flags_t flags) noexcept
    {
        this->onstart_flags = flags;
    }

    void set_pid_file(string &&pid_file) noexcept
    {
        this->pid_file = std::move(pid_file);
    }
    
    void set_socket_details(string &&socket_path, int socket_perms, uid_t socket_uid, uid_t socket_gid)
            noexcept
    {
        this->socket_path = std::move(socket_path);
        this->socket_perms = socket_perms;
        this->socket_uid = socket_uid;
        this->socket_gid = socket_gid;
    }

    // Set the service that this one "chains" to. When this service completes, the named service is started.
    void set_chain_to(string &&chain_to)
    {
        start_on_completion = std::move(chain_to);
    }

    const std::string &get_name() const noexcept { return service_name; }
    service_state_t get_state() const noexcept { return service_state; }
    
    void start(bool activate = true) noexcept;  // start the service
    void stop(bool bring_down = true) noexcept;   // stop the service
    bool restart() noexcept; // restart the service, returns true iff restart issued
    
    void forced_stop() noexcept; // force-stop this service and all dependents
    
    // Pin the service in "started" state (when it reaches the state)
    void pin_start() noexcept
    {
        pinned_started = true;
    }
    
    // Pin the service in "stopped" state (when it reaches the state)
    void pin_stop() noexcept
    {
        pinned_stopped = true;
    }
    
    // Remove both "started" and "stopped" pins. If the service is currently pinned
    // in either state but would naturally be in the opposite state, it will immediately
    // commence starting/stopping.
    void unpin() noexcept;
    
    // Is this a dummy service (used only when loading a new service)?
    bool is_dummy() noexcept
    {
        return record_type == service_type_t::DUMMY;
    }
    
    bool did_start_fail() noexcept
    {
        return start_failed;
    }

    bool was_start_skipped() noexcept
    {
        return start_skipped;
    }

    // Add a listener. A listener must only be added once. May throw std::bad_alloc.
    void add_listener(service_listener * listener)
    {
        listeners.insert(listener);
    }
    
    // Remove a listener.    
    void remove_listener(service_listener * listener) noexcept
    {
        listeners.erase(listener);
    }
    
    // Assuming there is one reference (from a control link), return true if this is the only reference,
    // or false if there are others (including dependents).
    bool has_lone_ref() noexcept
    {
        if (! dependents.empty()) return false;
        auto i = listeners.begin();
        return (++i == listeners.end());
    }

    // Prepare this service to be unloaded.
    void prepare_for_unload() noexcept
    {
        // Remove all dependencies:
        for (auto &dep : depends_on) {
            auto &dep_dpts = dep.get_to()->dependents;
            dep_dpts.erase(std::find(dep_dpts.begin(), dep_dpts.end(), &dep));
        }
        depends_on.clear();
    }

    // Why did the service stop?
    stopped_reason_t get_stop_reason()
    {
        return stop_reason;
    }

    bool is_waiting_for_console()
    {
        return waiting_for_console;
    }

    bool has_console()
    {
        return have_console;
    }

    virtual pid_t get_pid()
    {
        return -1;
    }

    virtual int get_exit_status()
    {
        return 0;
    }

    dep_list & get_dependencies()
    {
        return depends_on;
    }

    dpt_list & get_dependents()
    {
        return dependents;
    }

    // Add a dependency. Caller must ensure that the services are in an appropriate state and that
    // a circular dependency chain is not created. Propagation queues should be processed after
    // calling this. May throw std::bad_alloc.
    service_dep & add_dep(service_record *to, dependency_type dep_type)
    {
        depends_on.emplace_back(this, to, dep_type);
        try {
            to->dependents.push_back(& depends_on.back());
        }
        catch (...) {
            depends_on.pop_back();
            throw;
        }

        if (dep_type == dependency_type::REGULAR) {
            if (service_state == service_state_t::STARTING || service_state == service_state_t::STARTED) {
                to->require();
                depends_on.back().holding_acq = true;
            }
        }

        return depends_on.back();
    }

    // Remove a dependency, of the given type, to the given service. Propagation queues should be processed
    // after calling.
    void rm_dep(service_record *to, dependency_type dep_type) noexcept
    {
        for (auto i = depends_on.begin(); i != depends_on.end(); i++) {
            auto & dep = *i;
            if (dep.get_to() == to && dep.dep_type == dep_type) {
                for (auto j = to->dependents.begin(); ; j++) {
                    if (*j == &dep) {
                        to->dependents.erase(j);
                        break;
                    }
                }
                if (dep.holding_acq) {
                    to->release();
                }
                depends_on.erase(i);
                break;
            }
        }
    }

    // Start a speficic dependency of this service. Should only be called if this service is in an
    // appropriate state (started, starting). The dependency is marked as holding acquired; when
    // this service stops, the dependency will be released and may also stop.
    void start_dep(service_dep &dep)
    {
        if (! dep.holding_acq) {
            dep.get_to()->require();
            dep.holding_acq = true;
        }
    }
};

inline auto extract_prop_queue(service_record *sr) -> decltype(sr->prop_queue_node) &
{
    return sr->prop_queue_node;
}

inline auto extract_stop_queue(service_record *sr) -> decltype(sr->stop_queue_node) &
{
    return sr->stop_queue_node;
}

inline auto extract_console_queue(service_record *sr) -> decltype(sr->console_queue_node) &
{
    return sr->console_queue_node;
}

/*
 * A service_set, as the name suggests, manages a set of services.
 *
 * Other than the ability to find services by name, the service set manages various queues.
 * One is the queue for processes wishing to acquire the console. There is also a set of
 * processes that want to start, and another set of those that want to stop. These latter
 * two "queues" (not really queues since their order is not important) are used to prevent too
 * much recursion and to prevent service states from "bouncing" too rapidly.
 * 
 * A service that wishes to start or stop puts itself on the start/stop queue; a service that
 * needs to propagate changes to dependent services or dependencies puts itself on the
 * propagation queue. Any operation that potentially manipulates the queues must be followed
 * by a "process queues" order (processQueues() method).
 *
 * Note that processQueues always repeatedly processes both queues until they are empty. The
 * process is finite because starting a service can never cause services to stop, unless they
 * fail to start, which should cause them to stop semi-permanently.
 */
class service_set
{
    protected:
    int active_services;
    std::list<service_record *> records;
    bool restart_enabled; // whether automatic restart is enabled (allowed)
    
    shutdown_type_t shutdown_type = shutdown_type_t::NONE;  // Shutdown type, if stopping
    
    // Services waiting for exclusive access to the console
    dlist<service_record, extract_console_queue> console_queue;

    // Propagation and start/stop "queues" - list of services waiting for processing
    slist<service_record, extract_prop_queue> prop_queue;
    slist<service_record, extract_stop_queue> stop_queue;
    
    public:
    service_set()
    {
        active_services = 0;
        restart_enabled = true;
    }
    
    virtual ~service_set()
    {
        for (auto * s : records) {
            delete s;
        }
    }

    // Start the specified service. The service will be marked active.
    void start_service(service_record *svc)
    {
        svc->start();
        process_queues();
    }

    // Stop the specified service. Its active mark will be cleared.
    void stop_service(service_record *svc)
    {
        svc->stop(true);
        process_queues();
    }

    // Locate an existing service record.
    service_record *find_service(const std::string &name) noexcept;

    // Load a service description, and dependencies, if there is no existing
    // record for the given name.
    // Throws:
    //   service_load_exc (or subclass) on problem with service description
    //   std::bad_alloc on out-of-memory condition
    virtual service_record *load_service(const char *name)
    {
        auto r = find_service(name);
        if (r == nullptr) {
            throw service_not_found(name);
        }
        return r;
    }

    // Start the service with the given name. The named service will begin
    // transition to the 'started' state.
    //
    // Throws a service_load_exc (or subclass) if the service description
    // cannot be loaded or is invalid;
    // Throws std::bad_alloc if out of memory.
    void start_service(const char *name)
    {
        using namespace std;
        service_record *record = load_service(name);
        service_set::start_service(record);
    }
    
    void add_service(service_record *svc)
    {
        records.push_back(svc);
    }
    
    void remove_service(service_record *svc)
    {
        records.erase(std::find(records.begin(), records.end(), svc));
    }

    // Get the list of all loaded services.
    const std::list<service_record *> &list_services() noexcept
    {
        return records;
    }
    
    // Add a service record to the state propagation queue. The service record will have its
    // do_propagation() method called when the queue is processed.
    void add_prop_queue(service_record *service) noexcept
    {
        if (! prop_queue.is_queued(service)) {
            prop_queue.insert(service);
        }
    }
    
    // Add a service record to the stop queue. The service record will have its
    // execute_transition() method called when the queue is processed.
    void add_transition_queue(service_record *service) noexcept
    {
        if (! stop_queue.is_queued(service)) {
            stop_queue.insert(service);
        }
    }
    
    // Process state propagation and start/stop queues, until they are empty.
    void process_queues() noexcept
    {
        while (! stop_queue.is_empty() || ! prop_queue.is_empty()) {
            while (! prop_queue.is_empty()) {
                auto next = prop_queue.pop_front();
                next->do_propagation();
            }
            if (! stop_queue.is_empty()) {
                auto next = stop_queue.pop_front();
                next->execute_transition();
            }
        }
    }
    
    // Set the console queue tail (returns previous tail)
    void append_console_queue(service_record * newTail) noexcept
    {
        bool was_empty = console_queue.is_empty();
        console_queue.append(newTail);
        if (was_empty) {
            enable_console_log(false);
        }
    }
    
    // Pull and dispatch a waiter from the console queue
    void pull_console_queue() noexcept
    {
        if (console_queue.is_empty()) {
            // Discard the log buffer now, because we've potentially blocked output for a while
            // and allowed it to fill with stale messages. (If not much time has passed, the
            // request to discard will be ignored anyway).
            discard_console_log_buffer();
            enable_console_log(true);
        }
        else {
            service_record * front = console_queue.pop_front();
            front->acquired_console();
        }
    }
    
    void unqueue_console(service_record * service) noexcept
    {
        if (console_queue.is_queued(service)) {
            console_queue.unlink(service);
        }
    }

    // Check if console queue is empty (possibly due to console already having
    // been assigned to the only queueing service)
    bool is_console_queue_empty() noexcept
    {
        return console_queue.is_empty();
    }

    // Check whether a service is queued for the console
    bool is_queued_for_console(service_record * service) noexcept
    {
        return console_queue.is_queued(service);
    }

    // Notification from service that it is active (state != STOPPED)
    // Only to be called on the transition from inactive to active.
    void service_active(service_record *) noexcept;
    
    // Notification from service that it is inactive (STOPPED)
    // Only to be called on the transition from active to inactive.
    void service_inactive(service_record *) noexcept;
    
    // Find out how many services are active (starting, running or stopping,
    // but not stopped).
    int count_active_services() noexcept
    {
        return active_services;
    }
    
    void stop_all_services(shutdown_type_t type = shutdown_type_t::HALT) noexcept
    {
        restart_enabled = false;
        shutdown_type = type;
        for (std::list<service_record *>::iterator i = records.begin(); i != records.end(); ++i) {
            (*i)->stop(false);
            (*i)->unpin();
        }
        process_queues();
    }
    
    bool is_shutting_down() noexcept
    {
        return !restart_enabled;
    }

    shutdown_type_t get_shutdown_type() noexcept
    {
        return shutdown_type;
    }

    // Get an identifier for the run-time type of the service set (similar to typeid, but without
    // requiring RTTI to be enabled during compilation).
    virtual int get_set_type_id()
    {
        return SSET_TYPE_NONE;
    }
};

// A service directory entry, tracking the directory as a nul-terminated string, which may either
// be static or dynamically allocated (via new char[...]).
class service_dir_entry
{
    const char *dir;
    bool dir_dyn_allocd;  // dynamically allocated?

    public:
    service_dir_entry(const char *dir_p, bool dir_dyn_allocd_p) :
        dir(dir_p), dir_dyn_allocd(dir_dyn_allocd_p)
    { }

    ~service_dir_entry()
    {
        if (dir_dyn_allocd) {
            delete[] dir;
        }
    }

    const char *get_dir() const
    {
        return dir;
    }
};

// A service set which loads services from one of several service directories.
class dirload_service_set : public service_set
{
    std::vector<service_dir_entry> service_dirs; // directories containing service descriptions

    public:
    dirload_service_set() : service_set()
    {
        // nothing to do.
    }

    dirload_service_set(const dirload_service_set &) = delete;

    // Construct a dirload_service_set which loads services from the specified directory. The
    // directory specified can be dynamically allocated via "new char[...]" (dyn_allocd == true)
    // or statically allocated.
    dirload_service_set(const char *service_dir_p, bool dyn_allocd = false) : service_set()
    {
        service_dirs.emplace_back(service_dir_p, dyn_allocd);
    }

    // Append a directory to the list of service directories, so that it is searched last for
    // service description files.
    void add_service_dir(const char *service_dir_p, bool dyn_allocd = true)
    {
        service_dirs.emplace_back(service_dir_p, dyn_allocd);
    }

    int get_service_dir_count()
    {
        return service_dirs.size();
    }

    const char * get_service_dir(int n)
    {
        return service_dirs[n].get_dir();
    }

    service_record *load_service(const char *name) override;

    int get_set_type_id() override
    {
        return SSET_TYPE_DIRLOAD;
    }
};

#endif
