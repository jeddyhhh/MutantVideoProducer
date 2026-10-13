#include "transport.h"
#include "ui/McpServer.h"
#include <QTcpSocket>
#include <QHostAddress>
#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QJsonDocument>
#include <random>
#include <memory>
#include <vector>
int runTransportHardening(McpServer* server,QStringList& log) {
    int failures=0;server->stop();QString error;
    auto check=[&](bool ok,const QString& label){log<<QString("%1 transport %2").arg(ok?"PASS":"FAIL",label);if(!ok)++failures;};
    if(!server->start(0,&error)){check(false,error);return failures;}
    const auto port=server->port();
    auto events=[] {QCoreApplication::processEvents(QEventLoop::AllEvents,5);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);};
    auto exchange=[&](const QByteArray& raw,int chunk=0,bool mustClose=false) {
        QTcpSocket socket;socket.connectToHost(QHostAddress::LocalHost,port);QElapsedTimer timer;timer.start();
        while(socket.state()!=QAbstractSocket::ConnectedState&&timer.elapsed()<1000)events();
        if(socket.state()!=QAbstractSocket::ConnectedState)return QByteArray();
        if(chunk)for(qsizetype i=0;i<raw.size();i+=chunk){socket.write(raw.mid(i,chunk));socket.flush();events();}
        else socket.write(raw);
        QByteArray result;timer.restart();
        while(timer.elapsed()<1500){events();result+=socket.readAll();
            if(socket.state()==QAbstractSocket::UnconnectedState)break;
            if(!mustClose&&result.contains("\r\n\r\n")) {
                auto h=result.indexOf("\r\n\r\n");int size=0;
                for(auto line:result.left(h).split('\n'))if(line.toLower().startsWith("content-length:"))size=line.mid(15).trimmed().toInt();
                if(result.size()>=h+4+size)break;
            }
        }
        if(mustClose&&socket.state()!=QAbstractSocket::UnconnectedState)return QByteArray("NOT-CLOSED ")+result;
        return result;
    };
    const QByteArray body=R"({"jsonrpc":"2.0","id":17,"method":"ping"})";
    const QByteArray host="Host: 127.0.0.1:"+QByteArray::number(port)+"\r\n";
    auto packet=[&](const QByteArray& extra=QByteArray()){return "POST /mcp HTTP/1.1\r\n"+host+extra+"Content-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body;};
    for(int chunk:{1,2,7,31,128})check(exchange(packet(),chunk,true).startsWith("HTTP/1.1 200"),QString("valid request fragmented into %1-byte writes").arg(chunk));
    for(const auto& length:{"garbage","-1","2147483648","99999999999999999999999","1e3","+1",""}) {
        auto raw="POST /mcp HTTP/1.1\r\n"+host+"Content-Length: "+length+"\r\n\r\n"+body;
        auto result=exchange(raw,0,true);check(result.isEmpty()||result.startsWith("HTTP/1.1 400"),QString("refuses malformed length '%1' and closes").arg(length));
    }
    for(auto duplicate:{QByteArray("Content-Length: 1\r\n"),QByteArray("Content-Length: ")+QByteArray::number(body.size())+"\r\n",QByteArray("Transfer-Encoding: chunked\r\n"),QByteArray("Broken-header\r\n"),QByteArray(" Folded: header\r\n")}) {
        auto result=exchange(packet(duplicate),0,true);check(result.isEmpty()||result.startsWith("HTTP/1.1 400"),"ambiguous framing/header refused: "+QString::fromLatin1(duplicate.trimmed()));
    }
    auto giant=packet("X-Huge: "+QByteArray(70000,'x')+"\r\n");auto result=exchange(giant,0,true);
    check(result.isEmpty()||result.startsWith("HTTP/1.1 400"),"complete oversized header refused (delimiter already present)");
    auto malformed=packet();malformed.replace("HTTP/1.1\r\n","\r\n");result=exchange(malformed,0,true);
    check(result.isEmpty()||result.startsWith("HTTP/1.1 400"),"request line without HTTP version refused");
    for(auto method:{QByteArray("GET"),QByteArray("DELETE"),QByteArray("PATCH")}) {
        auto raw=packet();raw.replace("POST /mcp",method+" /mcp");
        auto response=exchange(raw,0,true);
        check(response.startsWith(method=="DELETE"?"HTTP/1.1 200":"HTTP/1.1 405"),"Connection: close honored for "+QString::fromLatin1(method));
    }
    {auto invalidJson=packet();invalidJson[invalidJson.size()-1]='!';check(exchange(invalidJson,0,true).startsWith("HTTP/1.1 400"),"invalid JSON honors Connection: close");}
    auto truncated=packet();truncated.chop(3);
    for(int i=0;i<300;++i){QTcpSocket s;s.connectToHost(QHostAddress::LocalHost,port);QElapsedTimer timer;timer.start();while(s.state()!=QAbstractSocket::ConnectedState&&timer.elapsed()<1000)events();s.write(truncated);s.flush();events();s.abort();events();}
    check(exchange(packet(),7,true).startsWith("HTTP/1.1 200"),"300 abandoned partial requests do not poison subsequent clients");
    // Two complete requests sharing one packet must keep response order and IDs.
    auto one=packet();one.replace("Connection: close\r\n","");auto two=packet();two.replace("\"id\":17","\"id\":18");
    auto pipelined=exchange(one+two,0,true);check(pipelined.count("HTTP/1.1 200")==2&&pipelined.indexOf("\"id\":17")<pipelined.indexOf("\"id\":18"),"pipelined requests preserve two separate ordered responses");
    std::mt19937 rng(846239);bool healthy=true;
    for(int i=0;i<200;++i){auto raw=packet();raw[qsizetype(rng()%uint32_t(raw.indexOf("\r\n\r\n")))]=char(rng()%128);exchange(raw,0,false);healthy &= exchange(packet(),31,true).startsWith("HTTP/1.1 200");}
    check(healthy,"200 seeded mutated HTTP headers leave server responsive");
    {
        QTcpSocket partial;partial.connectToHost(QHostAddress::LocalHost,port);QElapsedTimer time;time.start();
        while(partial.state()!=QAbstractSocket::ConnectedState&&time.elapsed()<1000)events();
        partial.write("POST /mcp HTTP/1.1\r\n");partial.flush();
        bool armed=false;time.restart();
        while(!armed&&time.elapsed()<1000){events();for(auto* timer:server->findChildren<QTimer*>("mcpRequestDeadline"))
            if(auto* socket=qobject_cast<QTcpSocket*>(timer->parent());socket&&socket->peerPort()==partial.localPort()&&timer->isActive()){timer->start(20);armed=true;}}
        time.restart();while(partial.state()!=QAbstractSocket::UnconnectedState&&time.elapsed()<1000)events();
        check(armed&&partial.state()==QAbstractSocket::UnconnectedState,"partial-request deadline closes a stalled client");
    }
    {
        std::vector<std::unique_ptr<QTcpSocket>> clients;
        for(int i=0;i<140;++i){auto s=std::make_unique<QTcpSocket>();s->connectToHost(QHostAddress::LocalHost,port);clients.push_back(std::move(s));events();}
        QElapsedTimer time;time.start();while(time.elapsed()<300)events();int connected=0;
        for(auto& s:clients)connected+=s->state()==QAbstractSocket::ConnectedState;
        check(connected<=128&&connected>0,"simultaneous idle-client count is bounded at 128");
        for(auto& s:clients)s->abort();events();
        check(exchange(packet(),31,true).startsWith("HTTP/1.1 200"),"server recovers after an idle-client saturation burst");
    }
    {
        // Simulate a progress event loop after a complete tool request, with a
        // second request only partly received. Its deadline must survive the tool.
        const auto activity=QObject::connect(server,&McpServer::activity,server,[&](const QString& name) {
            if(name!="get_project")return;
            for(auto* timer:server->findChildren<QTimer*>("mcpRequestDeadline"))if(timer->isActive())timer->start(5);
            QEventLoop loop;QTimer::singleShot(30,&loop,&QEventLoop::quit);loop.exec();
        });
        const QByteArray body=R"({"jsonrpc":"2.0","id":19,"method":"tools/call","params":{"name":"get_project","arguments":{}}})";
        QTcpSocket partial;partial.connectToHost(QHostAddress::LocalHost,port);QElapsedTimer time;time.start();
        while(partial.state()!=QAbstractSocket::ConnectedState&&time.elapsed()<1000)events();
        partial.write("POST /mcp HTTP/1.1\r\n"+host+"Content-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body+"POST /mcp HTTP/1.1\r\n");partial.flush();
        QByteArray response;bool armed=false;time.restart();
        while(time.elapsed()<1000){events();response+=partial.readAll();for(auto* timer:server->findChildren<QTimer*>("mcpRequestDeadline"))
            if(auto* socket=qobject_cast<QTcpSocket*>(timer->parent());socket&&socket->peerPort()==partial.localPort())armed=timer->isActive();
            if(response.startsWith("HTTP/1.1 200")&&armed)break;}
        check(response.startsWith("HTTP/1.1 200")&&armed,"partial pipeline deadline survives a nested tool progress loop");
        QObject::disconnect(activity);partial.abort();events();
    }
    server->stop();return failures;
}
