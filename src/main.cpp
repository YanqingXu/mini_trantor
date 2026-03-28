int main() {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, 10);

    EventLoop loop;

    Channel ch(listenfd);
    ch.setReadCallback([&]() {
        int connfd = accept(listenfd, nullptr, nullptr);
        printf("new connection: %d\n", connfd);
    });

    loop.updateChannel(&ch);
    loop.loop();
}
