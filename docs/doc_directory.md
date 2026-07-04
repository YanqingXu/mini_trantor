docs/
├── game_server_network_base_scope_boundary.md
├── game_server_network_base_phase_closure_audit.md
├── game_server_network_base_lifecycle_hardening.md
├── roadmap_game_server_network_base_execution_plan.md
│
├── 00_overview/
│   ├── 00_project_summary.md
│   ├── 01_architecture_overview.md
│   ├── 02_module_map.md
│   ├── 03_thread_model.md
│   ├── 04_reactor_model.md
│   └── 05_coroutine_model.md
│
├── 01_callflow/
│   ├── 01_tcp_server_start.md
│   ├── 02_new_connection_flow.md
│   ├── 03_message_receive_flow.md
│   ├── 04_message_send_flow.md
│   ├── 05_connection_close_flow.md
│   ├── 06_tcp_client_connect_flow.md
│   ├── 07_timer_trigger_flow.md
│   └── 08_coroutine_resume_flow.md
│
├── 02_modules/
│   ├── core/
│   │   ├── EventLoop.md
│   │   ├── Channel.md
│   │   ├── Poller.md
│   │   ├── EPollPoller.md
│   │   └── TimerQueue.md
│   │
│   ├── net/
│   │   ├── TcpServer.md
│   │   ├── TcpConnection.md
│   │   ├── TcpConnectionDetailHelpers.md
│   │   ├── Acceptor.md
│   │   ├── Connector.md
│   │   ├── TcpClient.md
│   │   └── Buffer.md
│   │
│   ├── thread/
│   │   ├── EventLoopThread.md
│   │   └── EventLoopThreadPool.md
│   │
│   ├── coroutine/
│   │   ├── Task.md
│   │   ├── Awaitable.md
│   │   ├── SleepAwaitable.md
│   │   ├── WhenAll.md
│   │   └── WhenAny.md
│   │
│   ├── advanced/
│   │   ├── DnsResolver.md
│   │   └── TlsContext.md
│   │
│   └── utils/
│       ├── InetAddress.md
│       └── Socket.md
│
├── 03_tests_analysis/
│   ├── unit/
│   ├── contract/
│   └── integration/
│
├── 04_design/
│   ├── 01_reactor_design.md
│   ├── 02_buffer_design.md
│   ├── 03_connection_lifecycle.md
│   ├── 04_coroutine_design.md
│   └── 05_threading_model.md
│
├── 05_issues/
│   ├── 01_code_smells.md
│   ├── 02_design_problems.md
│   └── 03_possible_bugs.md
│
├── 06_rewrite/
│   ├── 01_minimal_reactor.md
│   ├── 02_minimal_tcp_server.md
│   └── 03_coroutine_bridge.md
│
└── 07_ai_prompts/
    ├── class_analysis.md
    ├── callflow_analysis.md
    ├── test_analysis.md
    ├── refactor_analysis.md
    └── bug_analysis.md
