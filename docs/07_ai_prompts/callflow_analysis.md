请沿着【场景】分析 mini-trantor 的完整调用链：

例如：

* TcpServer 启动
* 新连接到来
* 收到消息
* 发送消息
* coroutine resume

要求：

【1. 起点】

* 从哪里开始（main / user API）

【2. 调用链（核心）】

* 按顺序列出函数调用链
* 格式如下：
  A::func → B::func → C::func

【3. 每一步说明】

* 每一步做了什么
* 数据如何流动
* 控制权如何转移

【4. 涉及类】

* 列出所有参与的类
* 各自职责

【5. 关键机制】

* 是否涉及 epoll / callback / coroutine
* 如何触发

【6. 时序图（文字版）】
用类似格式：

Client → Server → EventLoop → Channel → TcpConnection

【7. 总结】

* 这条链解决了什么问题
