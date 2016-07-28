﻿#include "rudp/rudp_send_buffer.h"
#include "revolver/base_timer_value.h"
#include "rudp/rudp_log_macro.h"
#include "rudp/rudp_ccc.h"

#include <math.h>

BASE_NAMESPACE_BEGIN_DECL

#define DEFAULT_RUDP_SEND_BUFFSIZE  4096
#define NAGLE_DELAY                 100

RUDPSendBuffer::RUDPSendBuffer()
    : net_channel_(NULL)
    , buffer_seq_(0)
    , dest_max_seq_(0)
    , cwnd_max_seq_(0)
    , max_loss_seq_(0)
    , buffer_size_(DEFAULT_RUDP_SEND_BUFFSIZE)
    , buffer_data_size_(0)
    , nagle_(false)
    , send_ts_(0)
    , ccc_(NULL)
    , passive_mode_(RUDP_MODE_NORMAL) {
    reset();
}

RUDPSendBuffer::~RUDPSendBuffer() {
    reset();
}

void RUDPSendBuffer::reset() {
    buffer_seq_ = rand() + 1;
    dest_max_seq_ = buffer_seq_ - 1;
    cwnd_max_seq_ = buffer_seq_;
    max_loss_seq_ = buffer_seq_;
    buffer_data_size_ = 0;
    buffer_size_ = DEFAULT_RUDP_SEND_BUFFSIZE;
    nagle_ = false;
    send_ts_ = CBaseTimeValue::get_time_value().msec();

    loss_set_.clear();

    for (SendWindowMap::iterator it = send_window_.begin(); it != send_window_.end(); ++ it)
        RETURN_SEND_SEG(it->second);

    send_window_.clear();

    for (SendDataList::iterator it = send_data_.begin(); it != send_data_.end(); ++ it)
        RETURN_SEND_SEG(*it);

    send_data_.clear();

    bandwidth_ = 0;
    bandwidth_ts_ = CBaseTimeValue::get_time_value().msec();
    rudp_id_ = 0;
}

int32_t RUDPSendBuffer::send(const uint8_t* data, int32_t data_size) {
    int32_t copy_pos = 0;
    int32_t copy_size = 0;
    uint8_t* pos = (uint8_t*)data;
    uint64_t now_timer = CBaseTimeValue::get_time_value().msec();

    if (!passive_mode_ && !send_data_.empty()) { //粘包
        RUDPSendSegment* last_seg = send_data_.back();
        if (last_seg != NULL && last_seg->data_size_ < MAX_SEGMENT_SIZE) {
            copy_size = MAX_SEGMENT_SIZE - last_seg->data_size_;
            if (copy_size > data_size)
                copy_size = data_size;

            memcpy(last_seg->data_ + last_seg->data_size_, pos, copy_size);

            copy_pos += copy_size;
            pos += copy_size;
            last_seg->data_size_ += copy_size;
        }
    }

    //分片
    while (copy_pos < data_size) {
        GAIN_SEND_SEG(last_seg);

        //设置初始化的的时刻
        last_seg->push_ts_ = now_timer;
        last_seg->seq_ = buffer_seq_;
        buffer_seq_ ++;

        //确定拷贝的块长度
        copy_size = (data_size - copy_pos);
        if (copy_size > MAX_SEGMENT_SIZE)
            copy_size = MAX_SEGMENT_SIZE;

        memcpy(last_seg->data_, pos, copy_size);

        copy_pos += copy_size;
        pos += copy_size;
        last_seg->data_size_ = copy_size;

        send_data_.push_back(last_seg);
        if (buffer_data_size_ + copy_pos > buffer_size_)
            break;
    }

    buffer_data_size_ += copy_pos;

    //尝试发送,立即发送
    //if(!nagle_)
    attempt_send(now_timer);

    return copy_pos;
}

void RUDPSendBuffer::on_ack(uint64_t seq) {
    //ID错误
    if (cwnd_max_seq_ < seq) {
        RUDP_WARNING("[" << rudp_id_ << "]seq[" << seq << "] id error, cwnd_max_seq: " << cwnd_max_seq_);
        return;
    }


    if (!send_window_.empty()) {
        RUDP_DEBUG("[" << rudp_id_ << "]recv ack, remove segment before latest seq[" << seq
                   << "] send windows size: " << send_window_.size());
        //删除窗口的数据片
        SendWindowMap::iterator it = send_window_.begin();
        while (it != send_window_.end() && it->first <= seq) {
            //删除丢包信息
            loss_set_.erase(it->first);

            //更新数据缓冲的大小
            if (buffer_data_size_ >  it->second->data_size_)
                buffer_data_size_ = buffer_data_size_ - it->second->data_size_;
            else
                buffer_data_size_ = 0;

            bandwidth_ += it->second->data_size_;

            RETURN_SEND_SEG(it->second);
            send_window_.erase(it ++);

            ccc_->add_recv(1);
        }
    }

    if (dest_max_seq_ < seq)
        dest_max_seq_ = seq;

    //尝试发送
    attempt_send(CBaseTimeValue::get_time_value().msec());

    check_buffer();
}

void RUDPSendBuffer::on_nack(uint64_t base_seq, const LossIDArray& loss_ids) {
    uint64_t seq = base_seq;
    uint64_t seq_no = base_seq;
    RUDPSendSegment* seg = NULL;
    uint32_t sz = loss_ids.size();

//    for (size_t i = 0; i < sz; ++i) {
//        for (uint64_t k = seq + 1; k < loss_ids[i] + base_seq; k++) {
//            SendWindowMap::iterator it = send_window_.find(k);
//            if (it != send_window_.end()) {
//                if (buffer_data_size_ > it->second->data_size_)
//                    buffer_data_size_ = buffer_data_size_ - it->second->data_size_;
//                else
//                    buffer_data_size_ = 0;
//
//                bandwidth_ += it->second->data_size_;
//
//                RETURN_SEND_SEG(it->second);
//                send_window_.erase(it);
//                ccc_->add_recv(1);
//                RUDP_SEND_DEBUG("del, seq = " << k << "%u\n");
//            }
//        }
//
//        seq = loss_ids[i] + base_seq;
//    }
//
//    if (loss_ids.size() > 0)
//        ccc_->on_loss(base_seq, loss_ids);

    string log_string = "[";
    char val[256];
    for (size_t i = 0; i < sz; ++i) {
        uint64_t seq_no = loss_ids[i] + base_seq;
        sprintf(val, "%u,", loss_ids[i]);
        log_string += val;
        SendWindowMap::iterator it = send_window_.find(seq_no);
        if (it != send_window_.end()) {
            uint64_t now_timer = (uint32_t)CBaseTimeValue::get_time_value().msec();
            seg = it->second;
            net_channel_->send_data(0, seg->seq_, seg->data_, seg->data_size_, now_timer);

            if (cwnd_max_seq_ < seg->seq_)
                cwnd_max_seq_ = seg->seq_;

            seg->last_send_ts_ = now_timer;
            seg->send_count_++;
            ccc_->add_resend();
            RUDP_SEND_DEBUG("nack resend pkg[" << seg->seq_ << "], send cnt: " << seg->send_count_);
        } else {
            RUDP_WARNING("can't find nack seq[" << seq_no << "]");
        }
    }
    log_string += "]";
    RUDP_DEBUG("recv nack base seq[" << base_seq << "], id: " << log_string);

    /*now_timer = CBaseTimeValue::get_time_value().msec();

    net_channel_->send_data(0, seg->seq_, seg->data_, seg->data_size_, now_timer);

    if (cwnd_max_seq_ < seg->seq_)
        cwnd_max_seq_ = seg->seq_;

    seg->last_send_ts_ = now_timer;
    seg->send_count_++;
    send_packet_number++;*/

    ////增加丢包信息
    //if (ccc_->get_rtt() > 20) {
    //    uint32_t sz = loss_ids.size();
    //    for (size_t i = 0; i < sz; ++i) {
    //        SendWindowMap::iterator it = send_window_.find(loss_ids[i] + base_seq);
    //        if (it != send_window_.end()) {
    //            if (buffer_data_size_ >  it->second->data_size_)
    //                buffer_data_size_ = buffer_data_size_ - it->second->data_size_;
    //            else
    //                buffer_data_size_ = 0;

    //            bandwidth_ += it->second->data_size_;

    //            RETURN_SEND_SEG(it->second);
    //            send_window_.erase(it++);

    //            ccc_->add_recv(1);
    //        }
    //    }
    //}

    on_ack(base_seq);
}

void RUDPSendBuffer::on_timer(uint64_t now_timer) {
    attempt_send(now_timer);
    check_buffer();
}

void RUDPSendBuffer::check_buffer() {
    buffer_size_ = core_max(buffer_size_, (ccc_->get_send_window_size() * MAX_SEGMENT_SIZE));
    //检查是否可以写
    if (buffer_data_size_ < buffer_size_ && net_channel_ != NULL)
        net_channel_->on_write();
}

void RUDPSendBuffer::check_segment() {
    uint64_t now_timer = CBaseTimeValue::get_time_value().msec();
    SendWindowMap::iterator map_it;
    RUDPSendSegment* seg = NULL;

    SendWindowMap::iterator end_it = send_window_.begin();
    uint32_t var_rtt = ccc_->get_rtt_var();
    for (map_it = send_window_.begin(); map_it != end_it;) {
        seg = map_it->second;
        if (now_timer - seg->push_ts_ > var_rtt * 5) {
            send_window_.erase(map_it++);
            RUDP_DEBUG("remove segment[" << seg->seq_ << "] from send windows")
        } else {
            ++ map_it;
        }
    }
}

//CCC控制清除接口
void RUDPSendBuffer::clear_loss() {
    loss_set_.clear();
}

uint32_t RUDPSendBuffer::get_threshold(uint32_t rtt) {
    uint32_t rtt_threshold = 10;
    uint32_t var_rtt = core_max(ccc_->get_rtt_var(), rtt / 16);
    if (rtt < 10)
        rtt_threshold = 3;
    else if (rtt < 30)
        rtt_threshold = rtt + var_rtt + 3;
    else if (rtt < 100)
        rtt_threshold = rtt + var_rtt + 10;
    else
        rtt_threshold = rtt + var_rtt;
    /*else if (rtt < 300)
        rtt_threshold = rtt + var_rtt;
        else
        rtt_threshold = (uint32_t)(rtt + var_rtt - rtt / 8);*/

    if (ccc_->get_rtt_var() > 50)
        rtt_threshold += ccc_->get_rtt_var();

    return rtt_threshold;
}

uint32_t RUDPSendBuffer::calculate_snd_size(uint64_t now_timer) {
    uint32_t ret;
    uint64_t timer;
    if (now_timer < send_ts_ + 10) {
        ret = 0;
    } else {
        timer = now_timer - send_ts_;
        ret = (uint32_t)(ccc_->get_send_window_size() * timer / get_threshold(ccc_->get_rtt()));

        if (ret)
            send_ts_ = now_timer;
    }


    return core_min(ret, ccc_->get_send_window_size());
}

void RUDPSendBuffer::attempt_send(uint64_t now_timer) {
    uint32_t cwnd_size;
    uint32_t rtt_threshold = get_threshold(ccc_->get_rtt());
    uint32_t resend_threshold = core_max(15, 2 * rtt_threshold);
    uint32_t ccc_cwnd_size = ccc_->get_send_window_size();
    uint32_t ccc_delay_size = ccc_cwnd_size / 8;
    RUDPSendSegment* seg = NULL;
    SendWindowMap::iterator map_it;
    //uint32_t lead_ts = core_min(100, rtt_threshold / 4);
    uint32_t lead_ts = rtt_threshold / 4;/*core_min(ccc_->get_rtt() / 4, rtt_threshold / 4)*/;
    uint32_t send_packet_number  = 0;
    uint32_t var_rtt = ccc_->get_rtt_var() * 3;

    cwnd_size = send_window_.size();
    uint32_t min_seq = 0;

    if (calculate_snd_size(now_timer) == 0)
        return;

    if (passive_mode_) {  // check the timeout segment
        check_segment();
    } else {
        if (cwnd_size > 0 && send_packet_number < ccc_cwnd_size) { //丢包队列为空，重发所有窗口中超时的分片
            SendWindowMap::iterator end_it = send_window_.end();

            for (map_it = send_window_.begin(); map_it != end_it; ++map_it) {
                seg = map_it->second;

                if (send_packet_number >= ccc_cwnd_size || seg->push_ts_ + rtt_threshold / 3 > now_timer)
                    break;

                uint64_t min_seq = send_window_.begin()->first;
                //if (ccc_->get_rtt() > 30 && min_seq + /*ccc_cwnd_size / 3 - 2*/core_max(ccc_delay_size, 8) > seg->seq_ &&
                //    seg->last_send_ts_ + var_rtt < now_timer
                //        && seg->send_count_ < 5/*|| seg->last_send_ts_ + resend_threshold < now_timer*/)
                if (ccc_->get_rtt() > 30 && min_seq + core_max(ccc_delay_size, 8) > seg->seq_ &&
                        seg->last_send_ts_ + lead_ts < now_timer || seg->last_send_ts_ + rtt_threshold < now_timer) {
                    now_timer = CBaseTimeValue::get_time_value().msec();

                    net_channel_->send_data(0, seg->seq_, seg->data_, seg->data_size_, now_timer);

                    if (cwnd_max_seq_ < seg->seq_)
                        cwnd_max_seq_ = seg->seq_;

                    seg->last_send_ts_ = now_timer;
                    seg->send_count_++;
                    send_packet_number++;

                    if (min_seq + ccc_cwnd_size / 3 > seg->seq_)
                        ccc_->add_resend();
                    RUDP_DEBUG("resend seq[" << seg->seq_ << "], send cnt: " << seg->send_count_);
                    //RUDP_SEND_DEBUG("resend seq = " << seg->seq_);
                }
            }
        }
    }


    //判断是否可以发送新的报文
    if (ccc_cwnd_size > send_packet_number && cwnd_size < ccc_cwnd_size) {
        while (!send_data_.empty()) {
            RUDPSendSegment* seg = send_data_.front();
            //判断NAGLE算法,NAGLE最少需要在100MS凑1024个字节报文
            if (cwnd_size > 0 && nagle_ && seg->push_ts_ + NAGLE_DELAY > now_timer && seg->data_size_ < MAX_SEGMENT_SIZE - 256)
                break;

            //判断发送窗口
            if (cwnd_size < ccc_cwnd_size) {
                send_data_.pop_front();
                send_window_.insert(SendWindowMap::value_type(seg->seq_, seg));
                //send_window_[seg->seq_] = seg;
                cwnd_size ++;

                now_timer = CBaseTimeValue::get_time_value().msec();
                seg->push_ts_ = now_timer;
                seg->last_send_ts_ = now_timer;
                seg->send_count_ = 1;

                net_channel_->send_data(0, seg->seq_, seg->data_, seg->data_size_, now_timer);
                if (cwnd_max_seq_ < seg->seq_)
                    cwnd_max_seq_ = seg->seq_;
                send_packet_number++;
                //RUDP_DEBUG("send data[" << seg->seq_ << "] " << seg->data_size_ << " bytes");
                /*RUDP_SEND_DEBUG("send seq = " << seg->seq_);*/
            } else
                break;
        }
    }
}

uint32_t RUDPSendBuffer::get_bandwidth() {
    uint32_t ret = 0;

    uint64_t cur_ts = CBaseTimeValue::get_time_value().msec();
    if (cur_ts > bandwidth_ts_)
        ret = static_cast<uint32_t>(bandwidth_ * 1000 / (cur_ts - bandwidth_ts_));
    else
        ret = bandwidth_ * 1000;


    bandwidth_ts_ = cur_ts;
    bandwidth_ = 0;

    return ret;
}


BASE_NAMESPACE_END_DECL
