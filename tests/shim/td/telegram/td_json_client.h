// Solo per i test/simulazione: stesse firme dell'header ufficiale di TDLib.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int td_create_client_id(void);
void td_send(int client_id, const char* request);
const char* td_receive(double timeout);
const char* td_execute(const char* request);
#ifdef __cplusplus
}
#endif
