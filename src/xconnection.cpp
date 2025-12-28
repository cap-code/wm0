#include "xconnection.h"
#include <iostream>

XConnection::XConnection() {
    conn_ = xcb_connect(nullptr,nullptr);

    if(xcb_connection_has_error(conn_)){
        std::cerr<<"Failed to connect to X server"<<std::endl;
    }

}

XConnection::~XConnection() {
    if (conn_){
        xcb_disconnect(conn_);
    }
}

bool XConnection::is_valid() const {
    return conn_ && !xcb_connection_has_error(conn_);
}
xcb_connection_t* XConnection::get() const{
    return conn_;
}