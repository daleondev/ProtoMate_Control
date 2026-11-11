#ifndef PROTOMATE_OPCUA_SAMPLE_MODEL_H
#define PROTOMATE_OPCUA_SAMPLE_MODEL_H
#include <open62541/server.h>
#ifdef __cplusplus
extern "C" {
#endif
#define OPCUA_SAMPLE_NAMESPACE "urn:protomate:opcua:sample"
typedef struct {
    UA_DateTime started;
    UA_DateTime counterEpoch;
    UA_Double setpoint;
    UA_UInt16 namespaceIndex;
} SampleModel;
UA_StatusCode sample_configure(UA_ServerConfig *config, UA_UInt16 port);
UA_StatusCode sample_model_install(UA_Server *server, SampleModel *model);
#ifdef __cplusplus
}
#endif
#endif
