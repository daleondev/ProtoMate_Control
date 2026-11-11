#include "Model.h"
#include <open62541/server_config_default.h>
#include <open62541/plugin/accesscontrol_default.h>
#include <math.h>

UA_StatusCode sample_configure(UA_ServerConfig *c, UA_UInt16 port) {
    UA_StatusCode result = UA_ServerConfig_setMinimalCustomBuffer(c, port, NULL, 8192, 8192);
    if(result != UA_STATUSCODE_GOOD) return result;
    /* This demo deliberately accepts anonymous identities only. */
    if(c->sessionPKI.clear) c->sessionPKI.clear(&c->sessionPKI);
    c->sessionPKI = (UA_CertificateGroup){0};
    result = UA_AccessControl_default(c, true, NULL, 0, NULL);
    if(result != UA_STATUSCODE_GOOD) return result;
    c->maxSessions = 1;
    c->maxSecureChannels = 3;
    c->maxSessionTimeout = 30000;
    c->tcpMaxMsgSize = 16384;
    c->tcpMaxChunks = 2;
    c->maxNodesPerRead = 32;
    c->maxNodesPerWrite = 16;
    c->maxNodesPerBrowse = 16;
    c->maxNodesPerMethodCall = 4;
    c->maxSubscriptions = 2;
    c->maxSubscriptionsPerSession = 2;
    c->maxMonitoredItems = 16;
    c->maxMonitoredItemsPerSubscription = 16;
    c->maxMonitoredItemsPerCall = 16;
    c->queueSizeLimits = (UA_UInt32Range){1, 4};
    c->samplingIntervalLimits.min = 100;
    c->publishingIntervalLimits.min = 100;
    c->maxPublishReqPerSession = 4;
    c->maxNotificationsPerPublish = 16;
    c->maxRetransmissionQueueSize = 4;
    UA_String_clear(&c->applicationDescription.applicationUri);
    c->applicationDescription.applicationUri = UA_STRING_ALLOC(OPCUA_SAMPLE_NAMESPACE);
    return c->applicationDescription.applicationUri.data ? UA_STATUSCODE_GOOD : UA_STATUSCODE_BADOUTOFMEMORY;
}

static UA_StatusCode readValue(UA_Server *server, const UA_NodeId *session, void *sessionContext,
    const UA_NodeId *node, void *context, UA_Boolean timestamp,
    const UA_NumericRange *range, UA_DataValue *value) {
    (void)server; (void)session; (void)sessionContext;
    if(range) return UA_STATUSCODE_BADINDEXRANGEINVALID;
    SampleModel *model = context;
    UA_DateTime now = UA_DateTime_nowMonotonic();
    UA_Double number;
    UA_UInt32 counter;
    UA_StatusCode result;
    const UA_String counterName = UA_STRING("Counter");
    const UA_String uptimeName = UA_STRING("Uptime");
    if(UA_String_equal(&node->identifier.string, &counterName)) {
        counter = (UA_UInt32)((now - model->counterEpoch) / UA_DATETIME_SEC);
        result = UA_Variant_setScalarCopy(&value->value, &counter, &UA_TYPES[UA_TYPES_UINT32]);
    } else {
        number = UA_String_equal(&node->identifier.string, &uptimeName)
            ? (UA_Double)(now - model->started) / UA_DATETIME_SEC : model->setpoint;
        result = UA_Variant_setScalarCopy(&value->value, &number, &UA_TYPES[UA_TYPES_DOUBLE]);
    }
    value->hasValue = result == UA_STATUSCODE_GOOD;
    if(timestamp) { value->hasSourceTimestamp = true; value->sourceTimestamp = UA_DateTime_now(); }
    return result;
}

static UA_StatusCode writeSetpoint(UA_Server *server, const UA_NodeId *session, void *sessionContext,
    const UA_NodeId *node, void *context, const UA_NumericRange *range, const UA_DataValue *value) {
    (void)server; (void)session; (void)sessionContext; (void)node;
    if(range) return UA_STATUSCODE_BADINDEXRANGEINVALID;
    if(!value->hasValue || !UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_DOUBLE]))
        return UA_STATUSCODE_BADTYPEMISMATCH;
    const UA_Double number = *(const UA_Double *)value->value.data;
    if(!isfinite(number) || number < -100 || number > 100) return UA_STATUSCODE_BADOUTOFRANGE;
    ((SampleModel *)context)->setpoint = number;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode resetCounter(UA_Server *server, const UA_NodeId *session, void *sessionContext,
    const UA_NodeId *method, void *methodContext, const UA_NodeId *object, void *objectContext,
    size_t inputSize, const UA_Variant *input, size_t outputSize, UA_Variant *output) {
    (void)server; (void)session; (void)sessionContext; (void)method; (void)object;
    (void)objectContext; (void)input; (void)output;
    if(inputSize || outputSize) return UA_STATUSCODE_BADINVALIDARGUMENT;
    ((SampleModel *)methodContext)->counterEpoch = UA_DateTime_nowMonotonic();
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode sample_model_install(UA_Server *server, SampleModel *model) {
    *model = (SampleModel){.started = UA_DateTime_nowMonotonic()};
    model->counterEpoch = model->started;
    const UA_UInt16 ns = UA_Server_addNamespace(server, OPCUA_SAMPLE_NAMESPACE);
    if(!ns) return UA_STATUSCODE_BADOUTOFMEMORY;
    model->namespaceIndex = ns;
    UA_ObjectAttributes object = UA_ObjectAttributes_default;
    object.displayName = UA_LOCALIZEDTEXT("en", "Sample");
    UA_NodeId parent = UA_NODEID_STRING(ns, "Sample");
    UA_StatusCode result = UA_Server_addObjectNode(server, parent, UA_NS0ID(OBJECTSFOLDER),
        UA_NS0ID(ORGANIZES), UA_QUALIFIEDNAME(ns, "Sample"), UA_NS0ID(BASEOBJECTTYPE), object, NULL, NULL);
    if(result != UA_STATUSCODE_GOOD) return result;
    char *names[] = {"Uptime", "Counter", "Setpoint"};
    for(size_t i = 0; i < 3; ++i) {
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        const UA_DataType *type = &UA_TYPES[i == 1 ? UA_TYPES_UINT32 : UA_TYPES_DOUBLE];
        attr.displayName = UA_LOCALIZEDTEXT("en", names[i]);
        attr.dataType = type->typeId;
        attr.valueRank = UA_VALUERANK_SCALAR;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | (i == 2 ? UA_ACCESSLEVELMASK_WRITE : 0);
        attr.minimumSamplingInterval = 100;
        UA_DataSource source = {.read = readValue, .write = i == 2 ? writeSetpoint : NULL};
        result = UA_Server_addDataSourceVariableNode(server, UA_NODEID_STRING(ns, names[i]), parent,
            UA_NS0ID(HASCOMPONENT), UA_QUALIFIEDNAME(ns, names[i]), UA_NS0ID(BASEDATAVARIABLETYPE),
            attr, source, model, NULL);
        if(result != UA_STATUSCODE_GOOD) return result;
    }
    UA_MethodAttributes method = UA_MethodAttributes_default;
    method.displayName = UA_LOCALIZEDTEXT("en", "ResetCounter");
    method.executable = true;
    method.userExecutable = true;
    return UA_Server_addMethodNode(server, UA_NODEID_STRING(ns, "ResetCounter"), parent,
        UA_NS0ID(HASCOMPONENT), UA_QUALIFIEDNAME(ns, "ResetCounter"), method, resetCounter,
        0, NULL, 0, NULL, model, NULL);
}
