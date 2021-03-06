//
//  main.cpp
//  port-forward
//
//  Created by wenxue on 13-10-29.
//  Copyright (c) 2013年 wenx. All rights reserved.
//

#include <iostream>
#include <thread>

#include <boost/asio.hpp>

template<typename T, typename P>
class acceptor : public std::enable_shared_from_this<acceptor<T,P>> {
    boost::asio::io_service &io_service_;
	boost::asio::ip::tcp::acceptor acceptor_;
    P context_;

public:
    acceptor(boost::asio::io_service &io_service, const char *addr, const int port, P &context)
    : io_service_(io_service)
    , acceptor_(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::from_string(addr), port))
    , context_(context) {}
    virtual ~acceptor() {}

public:
    void startup_async_accept() {
        auto peer(std::make_shared<boost::asio::ip::tcp::socket>(io_service_));
		acceptor_.async_accept(*peer,
                               std::bind(&acceptor::async_accept_handler,
                                         std::enable_shared_from_this<acceptor<T,P>>::shared_from_this(),
                                         peer, std::placeholders::_1));
	}

	void async_accept_handler(std::shared_ptr<boost::asio::ip::tcp::socket>  peer, const boost::system::error_code &ec) {
        startup_async_accept();
		if (ec) {
            printf("acceptor::async_accept_handler %d\n", ec.value());
		} else {
            static boost::asio::ip::tcp::no_delay option(true);
            peer->set_option(option);
            auto obj(std::make_shared<T>(io_service_, context_));
			obj->startup(peer);
		}
    }
};

typedef std::shared_ptr<boost::asio::ip::tcp::socket> shared_ptr_peer;

template<typename T>
class forward_peer : public std::enable_shared_from_this<forward_peer<T>> {

    typedef forward_peer<T> THIS_T;

public:
    forward_peer(boost::asio::io_service& io_service, T &ep)
    : io_service_(io_service)
    , forward_endport_(ep) {}

    ~forward_peer() {}

public:
    void async_connect_handler(shared_ptr_peer incoming_peer,
                               shared_ptr_peer forward_peer,
                               const boost::system::error_code& ec) {
        if (ec) {
            printf("forward_peer::async_connect_handler can not connect to forward target %d\n", ec.value());
        } else {

            auto xf = [this](shared_ptr_peer first, shared_ptr_peer second) {
                auto buf = std::make_shared<boost::asio::streambuf>(1024*1024);
                first->async_receive(buf->prepare(1024*16),
                                     std::bind<int>(&forward_peer::async_receive_handler,
                                                    std::enable_shared_from_this<THIS_T>::shared_from_this(),
                                                    first,
                                                    second,
                                                    buf,
                                                    std::placeholders::_1,
                                                    std::placeholders::_2));
            };

            xf(incoming_peer, forward_peer);
            xf(forward_peer, incoming_peer);
        }
    }

public:
    int startup(shared_ptr_peer incoming_peer) {
        auto forward_peer(std::make_shared<boost::asio::ip::tcp::socket>(io_service_));
        printf("foward_peer::startup incoming peer %08X\n", &*incoming_peer);
        printf("foward_peer::startup forward peer %08X\n", &*forward_peer);
        forward_peer->async_connect(forward_endport_,
                                    std::bind(&forward_peer::async_connect_handler,
                                              std::enable_shared_from_this<THIS_T>::shared_from_this(),
                                              incoming_peer,
                                              forward_peer,
                                              std::placeholders::_1));
        return 0;
    }

    int async_send_handler(shared_ptr_peer sender,
                           shared_ptr_peer receiver,
                           std::shared_ptr<boost::asio::streambuf> buffer,
                           boost::system::error_code ec,
                           size_t bytes_transferred) {
        if (ec) {
            printf("foward_peer::async_send_handler %d\n", ec.value());
        } else {
            printf("foward_peer::async_send_handler %08X %lu bytes sent\n", &*sender, bytes_transferred);
            buffer->consume(bytes_transferred);
            if (buffer->size()) {

                sender->async_send(boost::asio::buffer(buffer->data(), buffer->size()),
                                   std::bind<int>(&forward_peer::async_send_handler,
                                                  std::enable_shared_from_this<THIS_T>::shared_from_this(),
                                                  sender,
                                                  receiver,
                                                  buffer,
                                                  std::placeholders::_1,
                                                  std::placeholders::_2));
            } else {
                receiver->async_receive(buffer->prepare(1024*1024),
                                        std::bind<int>(&forward_peer::async_receive_handler,
                                                       std::enable_shared_from_this<THIS_T>::shared_from_this(),
                                                       receiver,
                                                       sender,
                                                       buffer,
                                                       std::placeholders::_1,
                                                       std::placeholders::_2));
            }
        }
        return 0;
    }

    int async_receive_handler(shared_ptr_peer receiver,
                              shared_ptr_peer sender,
                              std::shared_ptr<boost::asio::streambuf> buffer,
                              boost::system::error_code ec,
                              size_t bytes_transferred) {
        if (ec) {
            printf("foward_peer::async_receive_handler %d\n", ec.value());
        } else {
            printf("foward_peer::async_receive_handler %08X %u bytes received\n", &*receiver, bytes_transferred);

            buffer->commit(bytes_transferred);
            {
                char fn[1024];
                memset(fn, 0, sizeof(fn));
                sprintf(fn, "%x.bin", &*receiver);

                FILE *fp = fopen(fn, "ab");
                assert(fp);
                auto rc = fwrite(buffer->data().begin(), 1, buffer->size(), fp);
                assert(rc == buffer->size());
                fflush(fp);
                fclose(fp);
            }
            char buf[1024];
            sender->async_send(boost::asio::buffer(buffer->data(), buffer->size()),
                               std::bind<int>(&forward_peer::async_send_handler,
                                              std::enable_shared_from_this<THIS_T>::shared_from_this(),
                                              sender,
                                              receiver,
                                              buffer,
                                              std::placeholders::_1,
                                              std::placeholders::_2));
        }
        return 0;
    }

private:
    boost::asio::io_service &io_service_;
    T &forward_endport_;

};

int main(int argc, const char * argv[])
{
    if (argc < 5 || (argc - 1) % 4) {
        printf("USAGE:%s incoming-addr incoming-port forward-addr forward-port\n", argv[0]);
        return 0;
    }
    boost::asio::io_service io_service;

    auto index = 1;

    while (index < argc) {
        const auto incoming_addr = argv[index++];

        const auto incoming_port = atoi(argv[index++]);
        assert(incoming_port);

        const auto forward_addr = argv[index++];

        const auto forward_port = atoi(argv[index++]);
        assert(forward_port);

        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::address_v4::from_string(forward_addr), forward_port);
        auto x = std::make_shared<acceptor<forward_peer<boost::asio::ip::tcp::endpoint>,boost::asio::ip::tcp::endpoint>>(io_service,
                                                                                                                         incoming_addr,
                                                                                                                         incoming_port,
                                                                                                                         ep);
        x->startup_async_accept();
    }
    printf("startup running ...\n");
    for (auto i = 0; i < 4; ++i) {
        auto f = static_cast<std::size_t(boost::asio::io_service::*)()>(&boost::asio::io_service::run);
        std::thread(std::bind(f, &io_service)).detach();
    }
    io_service.run();
    return 0;
}

