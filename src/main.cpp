#include "xconnection.h"
#include <iostream>
#include <unordered_map>


struct Client {
    xcb_window_t frame;
    xcb_window_t window;
    int x , y;
    int width , height;
};

struct DragState {
    bool active = false;
    xcb_window_t frame = XCB_NONE;
    int start_root_x = 0;
    int start_root_y = 0;
    int start_x = 0;
    int start_y = 0;
};

int main() {
    XConnection conn;

    if (!conn.is_valid()){
        return 1;
    }

    std::cout<<"XConnection established \n"<<std::endl;


    const xcb_setup_t* setup = xcb_get_setup(conn.get());
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    xcb_screen_t* screen = it.data;

    std::unordered_map<xcb_window_t, Client> clients;
    DragState drag;


    std::cout<<"Screen size: "<<screen->width_in_pixels<<"x"<<screen->height_in_pixels<<"\n"<<std::endl;

    std::cout<<"Root winow id: "<<screen->root<<"\n"<<std::endl;


    uint32_t root_events[] = {
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
    };

    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(
                                conn.get(),
                                screen->root,
                                XCB_CW_EVENT_MASK,
                                root_events
                            );
    
    xcb_generic_error_t* error = xcb_request_check(conn.get(),cookie);

    if(error){
        std::cerr<<"Failed to get the ownership of root windown\n"<<std::endl;
        free(error);
        return 1;
    }


    xcb_flush(conn.get());

    std::cout<<"WM running ...\n"<<std::endl;


    while(true){
        xcb_generic_event_t* event = xcb_wait_for_event(conn.get());
        if(!event) break;

        uint8_t type = event->response_type & ~0x80;

        switch(type){
            case XCB_CONFIGURE_REQUEST: {
                auto* e = reinterpret_cast<xcb_configure_request_event_t*>(event);

                auto it = clients.find(e->window);
                if( it == clients.end()){
                    uint32_t values[7];
                    int i = 0;
                    
                    if (e->value_mask & XCB_CONFIG_WINDOW_X)
                        values[i++] = e->x;
                    if (e->value_mask & XCB_CONFIG_WINDOW_Y)
                        values[i++] = e->y;
                    if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
                        values[i++] = e->width;
                    if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
                        values[i++] = e->height;
                    if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
                        values[i++] = e->border_width;
                    if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING)
                        values[i++] = e->sibling;
                    if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
                        values[i++] = e->stack_mode;
                    
                    xcb_configure_window(
                        conn.get(),
                        e->window,
                        e->value_mask,
                        values
                    );
                    break;
                }

                Client& c = it->second;

                if (e->value_mask & XCB_CONFIG_WINDOW_X)
                    c.x = e->x;
                if (e->value_mask & XCB_CONFIG_WINDOW_Y)
                    c.y = e->y;
                if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
                    c.width = e->width;
                if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
                    c.height = e->height;

                uint32_t frame_vals[] = {
                        static_cast<uint32_t>(c.x),
                        static_cast<uint32_t>(c.y),
                        static_cast<uint32_t>(c.width),
                        static_cast<uint32_t>(c.height)
                    };
                
                xcb_configure_window(
                    conn.get(),
                    c.frame,
                    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                    frame_vals
                );
                xcb_configure_window(
                    conn.get(),
                    c.window,
                    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                    &frame_vals[2]
                );

                break;

            }
            case XCB_MAP_REQUEST: {
                std::cout<<"Map request received\n"<<std::endl;
                auto* e = reinterpret_cast<xcb_map_request_event_t*>(event);

                xcb_get_window_attributes_cookie_t attr_cookie = xcb_get_window_attributes(conn.get(),e->window);
                xcb_get_window_attributes_reply_t* attr = xcb_get_window_attributes_reply(conn.get(),attr_cookie,nullptr);

                if(!attr) break;

                if(attr->override_redirect){
                    free(attr);
                    xcb_map_window(conn.get(),e->window);
                    break;
                }

                free(attr);

                if (clients.find(e->window) != clients.end()){
                    break;
                }

                auto geom_cookie = xcb_get_geometry(conn.get(), e->window);
                auto* geom = xcb_get_geometry_reply(conn.get(),geom_cookie, nullptr);

                int x = geom ? geom->x :100;
                int y = geom ? geom->y :100;
                int width = geom ? geom->width :800;
                int height = geom ? geom->height :400;

                if (geom) free(geom);

                xcb_window_t frame = xcb_generate_id(conn.get());

                uint32_t frame_events[] = {
                    XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS  
                    | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION
                };

                xcb_create_window(
                    conn.get(),
                    XCB_COPY_FROM_PARENT,
                    frame,
                    screen->root,
                    x,y,width,height,
                    10,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual,
                    XCB_CW_EVENT_MASK,
                    frame_events
                );

                xcb_reparent_window(
                    conn.get(),
                    e->window,
                    frame,
                    0,0
                );


                xcb_grab_button(
                    conn.get(),
                    0,
                    e->window,
                    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                    XCB_GRAB_MODE_ASYNC,
                    XCB_GRAB_MODE_ASYNC,
                    XCB_NONE,
                    XCB_NONE,
                    XCB_BUTTON_INDEX_1,
                    XCB_MOD_MASK_ANY
                );

                xcb_map_window(conn.get(),frame);
                xcb_map_window(conn.get(),e->window);

                clients[e->window] = {frame , e->window, x, y, width, height};
                break;
            }

            case XCB_BUTTON_PRESS:{
                auto* e = reinterpret_cast<xcb_button_press_event_t*>(event);

                for(auto& [window,client] : clients){
                    if((e->event == client.frame)||(e->event == client.window)||(e->child == client.window)){

                        if(e->detail == 1){
                            drag.active = true;
                            drag.frame = client.frame;
                            drag.start_root_x = e->root_x;
                            drag.start_root_y = e->root_y;
    
                            auto geom_cookie = xcb_get_geometry(conn.get(),client.frame);
    
                            auto* geom = xcb_get_geometry_reply(conn.get(),geom_cookie,nullptr);
    
                            if (geom){
                                drag.start_x = geom->x;
                                drag.start_y = geom->y;
                                free(geom);
                            }   
                        }

                        xcb_set_input_focus(
                            conn.get(),
                            XCB_INPUT_FOCUS_POINTER_ROOT,
                            client.window,
                            XCB_CURRENT_TIME
                        );

                        uint32_t values[] = {
                            XCB_STACK_MODE_ABOVE
                        };
                        xcb_configure_window(
                            conn.get(),
                            client.frame,
                            XCB_CONFIG_WINDOW_STACK_MODE,
                            values
                        );

                        break;
                    }
                }
                break;
            }

            case XCB_DESTROY_NOTIFY:{
                auto* e = reinterpret_cast<xcb_destroy_notify_event_t*>(event);

                auto it = clients.find(e->window);

                if (it != clients.end()){
                    xcb_destroy_window(conn.get(),it->second.frame);
                    clients.erase(it);
                }
                break;
            }

            case XCB_MOTION_NOTIFY:{
                if(!drag.active) break;

                auto* e = reinterpret_cast<xcb_motion_notify_event_t*>(event);

                int dx = e->root_x - drag.start_root_x;
                int dy = e->root_y - drag.start_root_y;

                int new_x = drag.start_x + dx;
                int new_y = drag.start_y + dy;

                uint32_t values[] ={
                    static_cast<uint32_t>(new_x),
                    static_cast<uint32_t>(new_y)
                };

                xcb_configure_window(
                    conn.get(),
                    drag.frame,
                    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                    values
                );

                break;

            }

            case XCB_BUTTON_RELEASE: {
                    drag.active = false;
                    drag.frame = XCB_NONE;
                    break;
            }

            default: break;
        }
        xcb_flush(conn.get());
        free(event);
    }

    return 0;
}