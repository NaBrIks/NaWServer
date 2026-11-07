# SeMaps authoring report · 2026-10-18

Created through native SeMaps MCP MCP tools. Every listed placement was authored or placed by this task. All six views were rendered and visually reviewed; render_view reported no problems. The owner explicitly requested save; MCP save succeeded, doctor returned no findings, and semaps check exited 0. No C++ extractor is configured: these are authored architecture concepts.

## v_layers

View: http://127.0.0.1:54381/app/#v_layers

Placements (17):

- `v_layers#e_layer_app`
- `v_layers#e_routes`
- `v_layers#e_producer`
- `v_layers#e_wifi`
- `v_layers#e_layer_web`
- `v_layers#e_auth`
- `v_layers#e_server`
- `v_layers#e_ws`
- `v_layers#e_container`
- `v_layers#e_request`
- `v_layers#e_statics`
- `v_layers#e_layer_sdk`
- `v_layers#e_rtos`
- `v_layers#e_httpd`
- `v_layers#e_tcp`
- `v_layers#e_model_note`
- `v_layers#e_pending`

Visible relations (6):

- `r_producer_ws_call`
- `r_routes_request_call`
- `r_server_httpd_depends`
- `r_ws_httpd_depends`
- `r_httpd_tcp_data_flow`
- `r_routes_server_depends`

## v_context

View: http://127.0.0.1:54381/app/#v_context

Placements (13):

- `v_context#e_firmware`
- `v_context#e_library`
- `v_context#e_server`
- `v_context#e_ws`
- `v_context#e_auth`
- `v_context#e_statics`
- `v_context#e_wifi`
- `v_context#e_app_store`
- `v_context#e_routes`
- `v_context#e_producer`
- `v_context#e_browser`
- `v_context#e_monitor`
- `v_context#e_boundary_note`

Visible relations (5):

- `r_producer_ws_call`
- `r_browser_server_data_flow`
- `r_browser_ws_data_flow`
- `r_monitor_server_data_flow`
- `r_routes_server_depends`

## v_tasks

View: http://127.0.0.1:54381/app/#v_tasks

Placements (14):

- `v_tasks#e_task_main`
- `v_tasks#e_main_wait`
- `v_tasks#e_publish`
- `v_tasks#e_task_http`
- `v_tasks#e_http_select`
- `v_tasks#e_body_recv`
- `v_tasks#e_app_callback`
- `v_tasks#e_http_job`
- `v_tasks#e_socket_wait`
- `v_tasks#e_task_sdk`
- `v_tasks#e_event_task`
- `v_tasks#e_network_tasks`
- `v_tasks#e_tasks_note`
- `v_tasks#e_queue_note`

Visible relations (3):

- `r_publish_http_job_data_flow`
- `r_body_recv_app_callback_call`
- `r_http_job_socket_wait_call`

## v_flow

View: http://127.0.0.1:54381/app/#v_flow

Placements (12):

- `v_flow#e_flow_producer`
- `v_flow#e_flow_payload`
- `v_flow#e_flow_copy`
- `v_flow#e_flow_enqueue`
- `v_flow#e_flow_return`
- `v_flow#e_flow_httpd`
- `v_flow#e_flow_alive`
- `v_flow#e_flow_send`
- `v_flow#e_flow_free`
- `v_flow#e_flow_idle`
- `v_flow#e_flow_failure`
- `v_flow#e_flow_lifetime`

Visible relations (8):

- `r_flow_payload_flow_copy_call`
- `r_flow_copy_flow_enqueue_data_flow`
- `r_flow_enqueue_flow_return_data_flow`
- `r_flow_enqueue_flow_alive_event`
- `r_flow_alive_flow_send_data_flow`
- `r_flow_send_flow_free_data_flow`
- `r_flow_free_flow_idle_data_flow`
- `r_flow_alive_flow_free_data_flow`

## v_app

View: http://127.0.0.1:54381/app/#v_app

Placements (14):

- `v_app#e_app_setup_zone`
- `v_app#e_app_own`
- `v_app#e_app_register`
- `v_app#e_app_begin`
- `v_app#e_app_running_zone`
- `v_app#e_app_incoming`
- `v_app#e_app_outgoing`
- `v_app#e_app_return`
- `v_app#e_app_stop_zone`
- `v_app#e_app_quiesce`
- `v_app#e_app_stop`
- `v_app#e_app_destroy`
- `v_app#e_app_contract`
- `v_app#e_app_shutdown_gap`

Visible relations (6):

- `r_app_own_app_register_data_flow`
- `r_app_register_app_begin_call`
- `r_app_incoming_app_return_data_flow`
- `r_app_outgoing_app_return_data_flow`
- `r_app_quiesce_app_stop_data_flow`
- `r_app_stop_app_destroy_data_flow`

## v_access

View: http://127.0.0.1:54381/app/#v_access

Placements (16):

- `v_access#e_access_routes_zone`
- `v_access#e_access_public`
- `v_access#e_access_protected`
- `v_access#e_access_guards_zone`
- `v_access#e_principal`
- `v_access#e_authenticator`
- `v_access#e_authorizer`
- `v_access#e_access_app_zone`
- `v_access#e_public_readings`
- `v_access#e_private_commands`
- `v_access#e_access_data_zone`
- `v_access#e_user_store`
- `v_access#e_roles_catalog`
- `v_access#e_sessions_ram`
- `v_access#e_build_choice`
- `v_access#e_access_phase`

Visible relations (6):

- `r_access_public_public_readings_data_flow`
- `r_access_protected_authenticator_security`
- `r_authenticator_authorizer_security`
- `r_authorizer_private_commands_security`
- `r_authenticator_user_store_storage`
- `r_authorizer_roles_catalog_storage`

