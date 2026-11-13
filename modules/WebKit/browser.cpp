// One isolated Chromium view. stdin/stdout carry newline-delimited JSON;
// diagnostics use stderr. Cookie values and page bodies are never logged.
#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QSocketNotifier>
#include <QWebChannel>
#include <QWebEngineCookieStore>
#include <QWebEngineHttpRequest>
#include <QWebEngineLoadingInfo>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

static void emit_event(QJsonObject event) {
    QByteArray bytes = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    const char *p = bytes.constData();
    qsizetype left = bytes.size();
    while (left > 0) {
        const auto n = ::send(STDOUT_FILENO, p, size_t(left), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { QCoreApplication::quit(); return; }
        p += n; left -= n;
    }
}
class ScriptBridge : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE void postMessage(const QString &name, const QVariant &body) {
        emit_event({{"event", "message"}, {"name", name}, {"body", QJsonValue::fromVariant(body)}});
    }
};

static QJsonObject cookie_json(const QNetworkCookie &cookie) {
    QJsonObject out{{"Name", QString::fromUtf8(cookie.name())}, {"Value", QString::fromUtf8(cookie.value())},
        {"Domain", cookie.domain()}, {"Path", cookie.path()}, {"Secure", cookie.isSecure()},
        {"HTTPOnly", cookie.isHttpOnly()}};
    if (!cookie.isSessionCookie()) out.insert("Expires", double(cookie.expirationDate().toSecsSinceEpoch()));
    return out;
}
static QNetworkCookie read_cookie(const QJsonObject &o) {
    QNetworkCookie c(o["Name"].toString().toUtf8(), o["Value"].toString().toUtf8());
    c.setDomain(o["Domain"].toString()); c.setPath(o["Path"].toString());
    c.setSecure(o["Secure"].toBool()); c.setHttpOnly(o["HTTPOnly"].toBool());
    if (o.contains("Expires")) c.setExpirationDate(QDateTime::fromSecsSinceEpoch(qint64(o["Expires"].toDouble())));
    return c;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("machoman-webview");
    QWebEngineProfile profile; // in-memory storage, explicitly seeded by Cocoa
    QWebEngineView view;
    auto *page = new QWebEnginePage(&profile, &view);
    view.setPage(page);
    view.setWindowTitle("Web content");
    view.resize(640, 600);
    QWebChannel channel(page);
    ScriptBridge bridge;
    channel.registerObject("machoman", &bridge);
    page->setWebChannel(&channel, QWebEngineScript::MainWorld);
    QObject::connect(page, &QWebEnginePage::loadStarted, [&] { emit_event({{"event", "started"}}); });
    QObject::connect(page, &QWebEnginePage::loadFinished, [&](bool ok) {
        emit_event({{"event", "finished"}, {"ok", ok}, {"url", page->url().toString()}});
    });
    QObject::connect(page, &QWebEnginePage::loadingChanged, [&](const QWebEngineLoadingInfo &info) {
        if (info.status() == QWebEngineLoadingInfo::LoadFailedStatus)
            emit_event({{"event", "error"}, {"code", info.errorCode()}, {"message", info.errorString()}});
    });
    QObject::connect(page, &QWebEnginePage::urlChanged, [&](const QUrl &url) {
        emit_event({{"event", "url"}, {"url", url.toString()}});
    });
    QObject::connect(page, &QWebEnginePage::titleChanged, [&](const QString &title) {
        view.setWindowTitle(title.isEmpty() ? "Web content" : title);
        emit_event({{"event", "title"}, {"title", title}});
    });
    QObject::connect(profile.cookieStore(), &QWebEngineCookieStore::cookieAdded, [&](const QNetworkCookie &cookie) {
        emit_event({{"event", "cookie"}, {"cookie", cookie_json(cookie)}});
    });
    QObject::connect(profile.cookieStore(), &QWebEngineCookieStore::cookieRemoved, [&](const QNetworkCookie &cookie) {
        emit_event({{"event", "cookieRemoved"}, {"cookie", cookie_json(cookie)}});
    });
    QFile channel_source(":/qtwebchannel/qwebchannel.js");
    if (!channel_source.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "WebKit: JavaScript channel resource is unavailable\n");
        return 1;
    }
    const QString channel_js = QString::fromUtf8(channel_source.readAll());
    auto configure = [&](const QJsonObject &o) {
        const QString suffix = o["applicationName"].toString();
        const QString custom = o["userAgent"].toString();
        if (!custom.isEmpty()) profile.setHttpUserAgent(custom);
        else if (!suffix.isEmpty()) profile.setHttpUserAgent(profile.httpUserAgent() + ' ' + suffix);
        const auto cookies = o["cookies"].toArray();
        for (const auto &c : cookies) profile.cookieStore()->setCookie(read_cookie(c.toObject()));
        page->scripts().clear();
        // Install handlers immediately and queue posts until the channel is ready.
        QString js = channel_js + QStringLiteral(R"JS(
            (function() {
              const pending = []; let bridge = null;
              window.webkit = window.webkit || {};
              window.webkit.messageHandlers = window.webkit.messageHandlers || {};
              const post = (name, body) => {
                if (bridge) bridge.postMessage(name, body);
                else pending.push([name, body]);
              };
        )JS");
        for (const auto &name : o["handlers"].toArray()) {
            const QString literal = QString::fromUtf8(QJsonDocument(QJsonArray{name}).toJson(QJsonDocument::Compact));
            js += "{ const name = " + literal + "[0]; window.webkit.messageHandlers[name] = {postMessage: body => post(name, body)}; }\n";
        }
        js += QStringLiteral(R"JS(
              if (typeof qt !== 'undefined' && qt.webChannelTransport) {
                new QWebChannel(qt.webChannelTransport, channel => {
                  bridge = channel.objects.machoman;
                  for (const args of pending) bridge.postMessage(args[0], args[1]);
                });
              }
            })();
        )JS");
        QWebEngineScript bootstrap;
        bootstrap.setName("Cocoa script message handlers");
        bootstrap.setInjectionPoint(QWebEngineScript::DocumentCreation);
        bootstrap.setWorldId(QWebEngineScript::MainWorld);
        bootstrap.setRunsOnSubFrames(true); bootstrap.setSourceCode(js);
        page->scripts().insert(bootstrap);
        for (const auto &value : o["scripts"].toArray()) {
            const auto script = value.toObject();
            QWebEngineScript s;
            s.setSourceCode(script["source"].toString());
            s.setInjectionPoint(script["time"].toInt() == 0 ? QWebEngineScript::DocumentCreation : QWebEngineScript::DocumentReady);
            s.setWorldId(QWebEngineScript::MainWorld);
            s.setRunsOnSubFrames(!script["mainOnly"].toBool()); page->scripts().insert(s);
        }
    };
    QByteArray input;
    QSocketNotifier notifier(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, [&](QSocketDescriptor, QSocketNotifier::Type) {
        char bytes[8192];
        const auto n = ::recv(STDIN_FILENO, bytes, sizeof bytes, 0);
        if (n <= 0) { notifier.setEnabled(false); app.quit(); return; }
        input.append(bytes, qsizetype(n));
        if (input.size() > 8 * 1024 * 1024) { app.quit(); return; }
        qsizetype end;
        while ((end = input.indexOf('\n')) >= 0) {
            const QByteArray line = input.left(end); input.remove(0, end + 1);
            const auto o = QJsonDocument::fromJson(line).object();
            const QString op = o["op"].toString();
            if (qEnvironmentVariableIsSet("MACHOMAN_WEBKIT_TRACE"))
                fprintf(stderr, "WebKit helper: command %s\n", op.toUtf8().constData());
            if (op == "quit") { app.quit(); return; }
            if (op == "configure") configure(o);
            else if (op == "open") {
                QWebEngineHttpRequest request(QUrl(o["url"].toString()));
                if (o["method"].toString() == "POST") {
                    request.setMethod(QWebEngineHttpRequest::Post);
                    request.setPostData(QByteArray::fromBase64(o["body"].toString().toUtf8()));
                }
                const auto headers = o["headers"].toObject();
                for (auto it = headers.begin(); it != headers.end(); ++it)
                    request.setHeader(it.key().toUtf8(), it.value().toString().toUtf8());
                view.show(); page->load(request);
            } else if (op == "html") { view.show(); page->setHtml(o["html"].toString(), QUrl(o["base"].toString())); }
            else if (op == "js") {
                const QJsonValue id = o["id"];
                page->runJavaScript(o["source"].toString(), QWebEngineScript::MainWorld, [id](const QVariant &result) {
                    emit_event({{"event", "js"}, {"id", id}, {"result", QJsonValue::fromVariant(result)}});
                });
            } else if (op == "stop") page->triggerAction(QWebEnginePage::Stop);
            else if (op == "reload") page->triggerAction(QWebEnginePage::Reload);
            else if (op == "back") page->triggerAction(QWebEnginePage::Back);
            else if (op == "forward") page->triggerAction(QWebEnginePage::Forward);
            else if (op == "resize") view.resize(std::max(100, o["width"].toInt()), std::max(100, o["height"].toInt()));
            else if (op == "hide") view.hide();
            else if (op == "cookie") profile.cookieStore()->setCookie(read_cookie(o["cookie"].toObject()));
            else if (op == "deleteCookie") profile.cookieStore()->deleteCookie(read_cookie(o["cookie"].toObject()));
        }
    });
    emit_event({{"event", "ready"}});
    return app.exec();
}
#include "browser.moc"
