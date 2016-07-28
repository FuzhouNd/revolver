﻿/*************************************************************************************
*filename:	base_rector.h
*
*to do:		定义反应器接口
*Create on: 2012-05
*Author:	zerok
*check list:
*************************************************************************************/
#ifndef __BASE_REACTOR_H
#define __BASE_REACTOR_H

#include "revolver/base_namespace.h"
#include "revolver/base_typedef.h"
#include "revolver/timer_queue_t.h"
#include "revolver/object_pool.h"
#include "revolver/base_thread_mutex.h"

#include <map>
#include <vector>
using namespace std;

BASE_NAMESPACE_BEGIN_DECL

class CEventHandler;

typedef CTimerQueue_T<CEventHandler*, CTimerFunctor, BaseThreadMutex> TIMEQUEUE;

typedef struct ReactorEventHandlerInfo
{
    CEventHandler*	event_handler;
    int32_t			event_mask;
    bool			event_close_;

    ReactorEventHandlerInfo()
    {
        event_handler = NULL;
        event_mask = 0;
        event_close_ = true;
    };
}ReactorEventHandlerInfo;

typedef map<BASE_HANDLER, ReactorEventHandlerInfo*> ReactorEventHandlerMap;

typedef ObjectPool<ReactorEventHandlerInfo, HANDLER_POOL_SIZE> ReactorEventHandlerInfoPool;

class IMessageProcessor
{
public:
    //扫描内部队列
    virtual int32_t	processor() = 0;
};

class CReactor
{
public:
    CReactor(){ msg_proc_ = NULL; opened_ = 0; };
    virtual ~CReactor(){};
    
    void	set_message_processor(IMessageProcessor* proc){msg_proc_ = proc;};

    virtual int32_t open_reactor(uint32_t number_of_handlers) = 0;
    virtual int32_t close_reactor() = 0;

    virtual int32_t event_loop() = 0;
    virtual int32_t stop_event_loop() = 0;

    virtual	void print() {};

    uint8_t is_opened() const { return opened_; }
    void set_opend(uint8_t val) { opened_ = val; }

    //事件操作
    //添加一个事件的监听
    virtual int32_t register_handler(CEventHandler *handler, uint32_t masks) = 0;
    //删除一个事件的特定监听
    virtual int32_t remove_handler(CEventHandler *handler, uint32_t masks) = 0;
    //删除一个事件
    virtual int32_t delete_handler(CEventHandler *handler, bool del_event_obj = false) = 0;
    //定时器操作 
    //添加一个定时器
    virtual uint32_t set_timer(CEventHandler *event_handler, const void *act, uint32_t delay) = 0;
    //删除一个定时器
    virtual uint32_t cancel_timer(uint32_t timer_id, const void **act) = 0;

protected:
    IMessageProcessor* msg_proc_;
    uint8_t  opened_;
};

BASE_NAMESPACE_END_DECL
#endif

/*************************************************************************************/
