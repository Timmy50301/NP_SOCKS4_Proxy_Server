#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#define REQUEST_INDEX 7
#define REPLY_INDEX 5

using namespace std;
using namespace boost::asio::ip;
using namespace boost::asio;
io_context io;

class request
{
    public:
        enum cmdType { connect = 0x01, bind = 0x02 };
        unsigned char version_;
        unsigned char command_;
        unsigned char destPortHigh_;
        unsigned char destPortLow_;
        address_v4::bytes_type destIP_;
        string userID_;
        unsigned char Null_;
        unsigned char domainName_;
        unsigned char socks4aNull_;

        boost::array<mutable_buffer, REQUEST_INDEX> buffers()
        {
            boost::array<mutable_buffer, REQUEST_INDEX> request = {
                buffer(&version_, 1),
                buffer(&command_, 1),
                buffer(&destPortHigh_, 1),
                buffer(&destPortLow_, 1),
                buffer(destIP_),
                buffer(userID_),
                buffer(&Null_, 1)
            };
            return request;
        }
        boost::array<mutable_buffer, 1> socks4aBuffers()
        {
            boost::array<mutable_buffer, 1> request = {
                buffer(&domainName_, 1)
            };
            return request;
        }
        unsigned short GetPort()
        {
            unsigned short port = destPortHigh_;
            port = (port<<8) & 0xff00;
            port |= destPortLow_;
            return port;
        }
        bool IsSocks4a()
        {
            string destIP = address_v4(destIP_).to_string();    // 127.0.0.1
            vector<string> ipV;
            boost::split(ipV, destIP, boost::is_any_of("."), boost::token_compress_on);
            bool issocks4a = false;
            for(int i=0,n=0 ; i<ipV.size() ; i++,n++)
            {
                string subIP = ipV[i];
                if(n < 3 && subIP == "0")
                    issocks4a = true;
                else if(n == 3 && subIP.size() ==1 && subIP != "0")
                    issocks4a = true;
                else
                    return false;
            }
            return issocks4a;
        }
};
class reply
{
    public:
        enum statusType { Accept = 0x5a, Reject = 0x5b};
        reply(statusType status, request req)
            :version_(0), 
            status_(status),
            destPortHigh_(req.destPortHigh_),
            destPortLow_(req.destPortLow_),
            destIP_(req.destIP_) {}

        boost::array<const_buffer, REPLY_INDEX> buffers()
        {
            boost::array<const_buffer, REPLY_INDEX> reply = {
                buffer(&version_, 1),
                buffer(&status_, 1),
                buffer(&destPortHigh_, 1),
                buffer(&destPortLow_, 1),
                buffer(destIP_),
            };
            return reply;
        }
            
    private:
        unsigned char version_;
        unsigned char status_;
        unsigned char destPortHigh_;
        unsigned char destPortLow_;
        address_v4::bytes_type destIP_{};
};
class server
{
    public:
        server(unsigned short port) : acceptor_(io, tcp::endpoint(tcp::v4(), port))
        {
            signal_.async_wait(boost::bind(&server::WaitHandle, this)); //SignalWait
            acceptor_.async_accept(sourSocket_, boost::bind(&server::AcceptHandle, this, _1));  //Accept
        }
    private:
        tcp::acceptor acceptor_;
        tcp::resolver resolver_{io};
        tcp::socket sourSocket_{io};
        tcp::socket destSocket_{io};
        signal_set signal_{io};
        request req_;
        array<unsigned char, 65536> destBuf_{};
        array<unsigned char, 65536> sourBuf_{};
        string hostName_ = "";
        
        void WaitHandle()
        {
            if(acceptor_.is_open())
            {
                int status = 0;
                while(waitpid(-1, &status, WNOHANG)){}
                signal_.async_wait(boost::bind(&server::WaitHandle, this)); //SignalWait
            }
        }
        void AcceptHandle(const boost::system::error_code& ec)
        {
            if(!ec)
            {
                io.notify_fork(boost::asio::io_context::fork_prepare);
                pid_t pid = fork();
                if(pid == 0)            // child process
                {
                    io.notify_fork(boost::asio::io_context::fork_child);
                    signal_.cancel();
                    acceptor_.close();

                    string cmd;         // CONNECT or BIND
                    string destIP;
                    string destPort;
                    string status;      // Accept or Reject

                    RecvSock4Request();
                    GetDestInfo(cmd, destPort, destIP);
                    if(PassFirewall(cmd, destIP))
                    {
                        status = "Accept";
                        PrintMsg(destIP, destPort, cmd, status);
                        if(cmd == "Connect")
                        {
                            if(req_.IsSocks4a())
                            {
                                RecvSock4aRequest();
                                Resolve(tcp::resolver::query(hostName_, destPort));
                            }   
                            else
                                Resolve(tcp::resolver::query(destIP, destPort));
                        }
                        else        // cmd== "Bind"
                        {
                            tcp::acceptor bindAcceptor{io};
                            tcp::endpoint endpoint(tcp::v4(), 0);
                            
                            Bind(bindAcceptor, endpoint);

                            req_.destPortHigh_ = bindAcceptor.local_endpoint().port() >>8;
                            req_.destPortLow_ = bindAcceptor.local_endpoint().port();
                            req_.destIP_ = make_address_v4("0.0.0.0").to_bytes();
                            Reply();
                            bindAcceptor.accept(destSocket_);
                            Reply();
                            CommFromDest();
                            CommFromSour();
                        }
                    }
                    else            // didn't pass firewall
                    {
                        status = "Reject";
                        PrintMsg(destIP, destPort, cmd, status);
                        reply rep(reply::Reject, req_);
                        write(sourSocket_, rep.buffers());
                    }
                }
                else
                {
                    io.notify_fork(boost::asio::io_context::fork_parent);
                    sourSocket_.close();
                    destSocket_.close();
                    
                    acceptor_.async_accept(sourSocket_, boost::bind(&server::AcceptHandle, this, _1));  //Accept
                }
            }
        }
        void RecvSock4Request()
        {
            boost::system::error_code error;
            read(sourSocket_, req_.buffers(), error);

            if(error == error::eof)
                return; // Connection closed cleanly by peer.
            else if(error)
                throw boost::system::system_error(error); // Some other error.
        }
        void RecvSock4aRequest()
        {
            boost::system::error_code error;
            while(1)
            {
                read(sourSocket_, buffer(&req_.domainName_, 1), error);
                cout<<unsigned(req_.domainName_)<<" ";
                if(req_.domainName_ == 0x00)
                    break;
                hostName_ += char(req_.domainName_);
            }

            if(error == error::eof)
                return; // Connection closed cleanly by peer.
            else if(error)
                throw boost::system::system_error(error); // Some other error.
        }
        void GetDestInfo(string &cmd, string &destPort, string &destIP)
        {
            cmd = (req_.command_ == req_.connect) ? "Connect" : "Bind";
            destPort = to_string(req_.GetPort());
            destIP = address_v4(req_.destIP_).to_string();
        }
        bool PassFirewall(string cmd, string destIP)
        {
            ifstream firewallStream("./socks.conf");
            string line;
            string permitType = (cmd == "Connect") ? "permit c " : "permit b ";
            vector<string> permitV;

            while(getline(firewallStream, line))
            {
                if(line.substr(0, permitType.size()) == permitType)
                    permitV.push_back(line.substr(permitType.size()));
            }
            
            for(int i=0 ; i<permitV.size() ; i++)
            {
                string acceptIP = permitV[i].substr(0, permitV[i].find('*'));
                if(destIP.substr(0, acceptIP.size()) == acceptIP)
                    return true;
            }
            return false;
        }
        void PrintMsg(string destIP, string destPort, string cmd, string status)
        {
            cout<<"----------Proxy Message-----------"<<endl;
            cout<<"<S_IP>: "<<sourSocket_.remote_endpoint().address().to_string()<<endl;
            cout<<"<S_PORT>: "<<to_string(sourSocket_.remote_endpoint().port())<<endl;
            cout<<"<D_IP>: "<<destIP<<endl;
            cout<<"<D_PORT>: "<<destPort<<endl;
            cout<<"<Command>: "<<cmd<<endl;             // CONNECT or BIND
            cout<<"<Reply>: "<<status<<endl<<endl;      // Accept or Reject
        }
        void Resolve(tcp::resolver::query q)
        {
            resolver_.async_resolve(q,
                [this](const boost::system::error_code& ec, tcp::resolver::iterator it) 
                {
                    if (!ec)
                        Connect(it);
                });
        }
        void Connect(tcp::resolver::iterator it)
        {
            destSocket_.async_connect(*it, 
                [this](const boost::system::error_code& ec) 
                {
                    if(!ec) 
                    {
                        Reply();
                        CommFromDest();
                        CommFromSour();
                    }
                });
        }
        void Bind(tcp::acceptor &bindAcceptor, tcp::endpoint &endpoint)
        {
            bindAcceptor.open(endpoint.protocol());
            bindAcceptor.bind(endpoint);
            bindAcceptor.listen();
            bindAcceptor.local_endpoint().port();
        }
        void Reply()
        {
            reply rep(reply::Accept, req_);
            // rep.PrintReply();
            write(sourSocket_, rep.buffers());
        }
        void CommFromDest()
        {
            destSocket_.async_receive(buffer(destBuf_),
                [this](boost::system::error_code ec, size_t length) 
                {
                    if(!ec || ec == boost::asio::error::eof) 
                        CommToSour(length);
                    else
                        throw system_error{ec};
                });
        }
        void CommToSour(size_t length)
        {
            async_write(sourSocket_, buffer(destBuf_, length),
                [this](boost::system::error_code ec, size_t length) 
                {
                    if(!ec)
                        CommFromDest();
                    else
                        throw system_error{ec};
                });
        }
        void CommFromSour()
        {
            sourSocket_.async_receive(buffer(sourBuf_),
                [this](boost::system::error_code ec, size_t length) 
                {
                    if(!ec || ec == boost::asio::error::eof) 
                        CommToDest(length);
                    else
                        throw system_error{ec};
                });
        }
        void CommToDest(size_t length)
        {
            async_write(destSocket_, buffer(sourBuf_, length),
                [this](boost::system::error_code ec, size_t length) 
                {
                    if(!ec)
                        CommFromSour();
                    else
                        throw system_error{ec};
                });
        }
};
int main(int argc, char* const argv[])
{
    // signal(SIGCHLD, SIG_IGN);
    try
    {
        if(argc != 2)
        {
            cout<<"Usage: ./socks_server <port>\n";
            return 1;
        }

        server s(atoi(argv[1]));    //port == atoi(argv[1])
        io.run();
    }
    catch(std::exception& e)
    {
        cerr<<"Exception: "<<e.what()<<"\n";
    }

    return 0;
}