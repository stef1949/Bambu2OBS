#pragma once

/* Tools-menu connection settings. Credentials are entered here and go straight
 * to the credential store; sources only ever hold a profile id. */

#ifdef __cplusplus
extern "C" {
#endif

void bambu_settings_register(void);

/* Safe from any thread: the dialog itself always opens on the UI thread. */
void bambu_settings_open(void);
void bambu_settings_export_diagnostics(void);

/* Must run before the module unloads. Qt's main window can outlive the plugin, and
 * a dialog left in it would then call into unloaded code. */
void bambu_settings_shutdown(void);

/* For tests: points Test Connection at a local fake printer. */
void bambu_settings_set_ports(int mqtt_port, int ftps_port);

/* Implemented by the source: its properties list the printers. */
void bambu_overlay_refresh_properties(void);

#ifdef __cplusplus
}
#endif
