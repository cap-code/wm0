#pragma once

#include<xcb/xcb.h>

class XConnection {
    public:
    XConnection();
    ~XConnection();


    XConnection(const XConnection&) = delete;
    XConnection& operator=(const XConnection&) = delete;

    xcb_connection_t* get() const;
    bool is_valid() const;

    private:
    xcb_connection_t* conn_;

};
