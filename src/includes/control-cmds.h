// Dinit control command packet types

// Requests:

// Query protocol version:
constexpr static int DINIT_CP_QUERYVERSION = 0;

// Find (but don't load) a service:
constexpr static int DINIT_CP_FINDSERVICE = 1;

// Find or load a service:
constexpr static int DINIT_CP_LOADSERVICE = 2;

// Start or stop a service:
constexpr static int DINIT_CP_STARTSERVICE = 3;
constexpr static int DINIT_CP_STOPSERVICE  = 4;
constexpr static int DINIT_CP_WAKESERVICE = 5;
constexpr static int DINIT_CP_RELEASESERVICE = 6;

constexpr static int DINIT_CP_UNPINSERVICE = 7;

// List services:
constexpr static int DINIT_CP_LISTSERVICES = 8;

// Unload a service:
constexpr static int DINIT_CP_UNLOADSERVICE = 9;

// Shutdown:
constexpr static int DINIT_CP_SHUTDOWN = 10;
 // followed by 1-byte shutdown type

// Add/remove dependency to existing service:
constexpr static int DINIT_CP_ADD_DEP = 11;
constexpr static int DINIT_CP_REM_DEP = 12;

// Query service load path / mechanism:
constexpr static int DINIT_CP_QUERY_LOAD_MECH = 13;

// Add a waits for dependency from one service to another, and start the dependency:
constexpr static int DINIT_CP_ENABLESERVICE = 14;

// Find the name of a service (from a handle)
constexpr static int DINIT_CP_QUERYSERVICENAME = 15;

// Replies:

// Reply: ACK/NAK to request
constexpr static int DINIT_RP_ACK = 50;
constexpr static int DINIT_RP_NAK = 51;

// Request was bad (connection will be closed)
constexpr static int DINIT_RP_BADREQ = 52;

// Connection being closed due to out-of-memory condition
constexpr static int DINIT_RP_OOM = 53;

// Start service replies:
constexpr static int DINIT_RP_SERVICELOADERR = 54;
constexpr static int DINIT_RP_SERVICEOOM = 55; // couldn't start due to out-of-memory

constexpr static int DINIT_RP_SSISSUED = 56;  // service start/stop was issued (includes 4-byte service handle)
constexpr static int DINIT_RP_SSREDUNDANT = 57;  // service was already started/stopped (or for stop, not loaded)

// Query version response:
constexpr static int DINIT_RP_CPVERSION = 58;

// Service record loaded/found
constexpr static int DINIT_RP_SERVICERECORD = 59;
//     followed by 4-byte service handle, 1-byte service state

// Couldn't find/load service
constexpr static int DINIT_RP_NOSERVICE = 60;

// Service is already started/stopped
constexpr static int DINIT_RP_ALREADYSS = 61;

// Information on a service / list complete:
constexpr static int DINIT_RP_SVCINFO = 62;
constexpr static int DINIT_RP_LISTDONE = 63;

// Service loader information:
constexpr static int DINIT_RP_LOADER_MECH = 64;

// Dependent services prevent stopping/restarting. Includes size_t count, handle_t * N handles.
constexpr static int DINIT_RP_DEPENDENTS = 65;

// Service name:
constexpr static int DINIT_RP_SERVICENAME = 66;

// Information:

// Service event occurred (4-byte service handle, 1 byte event code)
constexpr static int DINIT_IP_SERVICEEVENT = 100;
