#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) != "--client")
        return 2;
    std::string line;
    if (!std::getline(std::cin, line))
        return 3;
    const auto input = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    QFile capture(input.value("key_file").toString() + ".fixture.json");
    if (!capture.open(QIODevice::WriteOnly))
        return 4;
    capture.write(QJsonDocument(input).toJson());
    capture.close();
    const auto mode = input.value("relay_token").toString();
    if (mode == "oversized")
        std::cout << std::string(16 * 1024 + 1, 'x') << '\n' << std::flush;
    else if (mode == "malformed")
        std::cout << "not json\n" << std::flush;
    else if (mode == "unsafe")
        std::cout << "{\"type\":\"ready\",\"url\":\"https://192.0.2.1:443\"}\n" << std::flush;
    else if (mode == "error-secret")
        std::cout << "{\"type\":\"error\",\"message\":\"error-secret rejected\"}\n" << std::flush;
    else
    {
        if (mode == "delayed")
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        std::cerr << std::string(20000, 'e') << std::flush;
        QTcpServer listener;
        if (!listener.listen(QHostAddress::LocalHost, 0))
            return 5;
        const auto ready = QJsonDocument(QJsonObject{{"type", "ready"},
            {"url", "https://127.0.0.1:" + QString::number(listener.serverPort())}}).toJson(QJsonDocument::Compact);
        std::cout << ready.constData() << "\n{\"type\":\"path\",\"path\":\"direct\"}\n" << std::flush;
        while (std::getline(std::cin, line)) {}
        return 0;
    }
    // The parent owns this fixture through its open input pipe.
    while (std::getline(std::cin, line)) {}
    return 0;
}
