/*
 * @Description: 
 * @Version: 1.0
 * @Author: Zhang AO
 * @studentID: 518021910368
 * @School: SJTU
 * @Date: 2021-03-04 19:32:31
 * @LastEditors: Seven
 * @LastEditTime: 2021-03-13 16:02:47
 */
/*
 * FILE: rdt_sender.cc
 * DESCRIPTION: Reliable data transfer sender.
 * NOTE: This implementation assumes there is no packet loss, corruption, or 
 *       reordering.  You will need to enhance it to deal with all these 
 *       situations.  In this implementation, the packet format is laid out as 
 *       the following:
 *       
 *       |<-  1 byte  ->|<-             the rest            ->|
 *       | payload size |<-             payload             ->|
 *
 *       The first byte of each packet indicates the size of the payload
 *       (excluding this single-byte header)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cassert>

#include "rdt_struct.h"
#include "rdt_sender.h"

#include <queue>

#define TIMER 0.3
//seq number最大为60，设计成window size的整数倍
//所以可以直接用seq%10得出当前packet 应该放在window中的哪一个位置
#define MAX_SEQ 60 //2^6-1 一个byte中用6个bit表示seq，最高位表示一个msg的开始，次高位表示一个msg的结束
#define MAX_WINDOW_SIZE 10
#define HEADER_SIZE 5
#define A_CONST 7 //放在包头中作为checksum的参考
// #define MAX_BUFFER_SIZE 128
#define MAX_PAYLOAD_SIZE (RDT_PKTSIZE - HEADER_SIZE)

/*
*包头设计：
*   |<- checksum ->|<-payload size->|<-sequence number->|<- a constant ->|
*   |<-  2 byte  ->|<-   1 byte   ->|<-     1 byte    ->|<-   1 byte   ->|
*/

using namespace std;

struct PacketInWindow
{
    bool if_ack = false;
    char seq = 0;
    packet *pkt = NULL;
    PacketInWindow() {}
    ~PacketInWindow()
    {
        if (pkt)
        {
            free(pkt);
            pkt = NULL;
        }
    }
};

static queue<message *>
    buffers;                                    //存message的缓存
static char next_packet_seq;                    //下一个要打包的包序列 for pack_and_load
static char next_send_packet_seq;               //下一个要发送的包的包序列 for sender_send
static PacketInWindow windows[MAX_WINDOW_SIZE]; //存放windowsize个packet
static int window_cursor;                       //windows中滑动的cursor，指向当前window的首位地址
static int n_pkt_window;                        //当前windows中的packet个数
static int msg_cursor;                          //为0时表示需要取一个新的message打包

/**
 * @description: 因特网checksum算法
 * @param {structpacket} *p
 * @return {*}
 * @author: Zhang Ao
 */
static short checksum(struct packet *p)
{
    // printf("reciever checksum\n");
    unsigned long c_sum = 0;
    for (int i = 2; i < RDT_PKTSIZE; i += 2) //除去checksum位的数据，其他位的数据每两个byte求和
    {
        c_sum += *(short *)(&p->data[i]);
    }
    while (c_sum >> 16) //高位byte与地位byte相加
        c_sum = (c_sum >> 16) + (c_sum & 0xffff);
    return ~c_sum;
}

/**
 * @description: 增加sequence number
 * @param {char} &seq
 * @return {*}
 * @author: Zhang Ao
 */
static void Seq_increase(char &seq)
{
    seq = (seq + 1) % MAX_SEQ;
}

/**
 * @description: 将buffer中缓存的message打包放入windows中
 * @param {*}
 * @return {*}
 * @author: Zhang Ao
 */
static void Pack_and_Load()
{
    printf("begin fill windows\n");
    if (buffers.empty())
    {
        if (n_pkt_window == 0) //也没有packet要发了，直接stop timer。
            Sender_StopTimer();
        return;
    }

    message *msg = buffers.front();
    assert(msg->size >= 0);
    // PacketInWindow *pkt;
    packet *p;
    // short sum;
    //windows没满并且buffer中还有message
    while (n_pkt_window < MAX_WINDOW_SIZE && buffers.size() > 0)
    {
        int index = (window_cursor + n_pkt_window) % MAX_WINDOW_SIZE;
        //保证此包是空的！
        assert(windows[index].pkt == NULL);
        //创建新的packet空间
        p = (packet *)malloc(RDT_PKTSIZE);
        windows[index].pkt = p;
        if (msg->size - msg_cursor > MAX_PAYLOAD_SIZE) //肯定不是包尾
        {                                              //可以填满一个packet
            p->data[2] = (char)MAX_PAYLOAD_SIZE;
            //如果cursor指向0，表示是一个msg的头，新msg的第一个packet需要将seq的最高位设置成1
            windows[index].seq = next_packet_seq;
            assert(next_packet_seq % MAX_WINDOW_SIZE == index);
            p->data[3] = (char)(msg_cursor != 0 ? next_packet_seq : next_packet_seq | 128);
            p->data[4] = A_CONST;
            memcpy(p->data + HEADER_SIZE, msg->data + msg_cursor, MAX_PAYLOAD_SIZE);
            //设置校验码
            *(short *)p->data = checksum(p);
            msg_cursor += MAX_PAYLOAD_SIZE;
            Seq_increase(next_packet_seq);
            n_pkt_window++;
        }
        else if (msg->size > msg_cursor) //这里肯定是一个msg的最后一个packet
        {
            p->data[2] = (char)(msg->size - msg_cursor);
            assert(p->data[2] > 0);
            windows[index].seq = next_packet_seq;
            assert(next_packet_seq % MAX_WINDOW_SIZE == index);
            //因为是此message最后一个包，让seq的次高位置为1
            p->data[3] = (char)(msg_cursor != 0 ? next_packet_seq | 64 : next_packet_seq | 192);
            p->data[4] = A_CONST;
            memcpy(p->data + HEADER_SIZE, msg->data + msg_cursor, p->data[2]);
            //设置校验码
            *(short *)p->data = checksum(p);
            msg_cursor = 0;                //下一次取新的msg，cursor重新指回0；
            Seq_increase(next_packet_seq); //增加seq
            n_pkt_window++;
            //释放存储完毕的message空间
            free(msg->data);
            msg->data = NULL;
            free(msg);
            msg = NULL;
            buffers.pop();

            //让msg指向新的地方
            msg = buffers.front();
        }
    }
}

/**
 * @description: 将windows现有的packet发送给reciever
 * @param {*}
 * @return {*}
 * @author: Zhang Ao
 */
static void Sender_Send()
{
    printf("sender send packets\n");
    PacketInWindow *p;
    for (int i = 0; i < n_pkt_window; i++)
    {
        p = &windows[(window_cursor + i) % MAX_WINDOW_SIZE];
        assert(p->pkt);
        //当前seq number比下一个要发送的包的seq小（分两种情况，一种是直观小，一种是next——send——packet因为溢出数值变小了的情况）
        //或者当前packet已经获得了ack，就不需要再发了
        if (p->seq < next_send_packet_seq || p->seq - next_send_packet_seq > 20)
            continue;
        else if (!p->if_ack) //此包还没收到ack
        {
            //sender这里不能free，因为还没收到ack的packet就还要保存在windows中
            assert(p->pkt->data[2] > 0 && p->pkt->data[4] == A_CONST);
            assert((p->pkt->data[3] & 63) == next_send_packet_seq);
            Sender_ToLowerLayer(p->pkt);
            printf("sender send a packet with seq=%d\n", next_send_packet_seq);
        }
        Seq_increase(next_send_packet_seq); //增加下一个要发送的seq
    }
}

/* sender initialization, called once at the very beginning */
void Sender_Init()
{
    next_packet_seq = 0;
    window_cursor = 0;
    n_pkt_window = 0;

    msg_cursor = 0;

    fprintf(stdout, "At %.2fs: sender initializing ...\n", GetSimulationTime());
}

/* sender finalization, called once at the very end.
   you may find that you don't need it, in which case you can leave it blank.
   in certain cases, you might want to take this opportunity to release some 
   memory you allocated in Sender_init(). */
void Sender_Final()
{
    printf("sender final\n");
    assert(buffers.size() == 0);
    fprintf(stdout, "At %.2fs: sender finalizing ...\n", GetSimulationTime());
}

/* event handler, called when a message is passed from the upper layer at the 
   sender */

void Sender_FromUpperLayer(struct message *msg)
{
    //使用 selective repeat策略
    //在buffer中缓存
    printf("sender from upper\n");
    message *m = (message *)malloc(sizeof(message));

    m->size = msg->size;
    m->data = (char *)malloc(m->size);
    memcpy(m->data, msg->data, m->size);
    buffers.push(m);
    //已经定时过了，直接return
    if (Sender_isTimerSet())
        return;
    //还没有计时器，开启计时器，并不断循环打包以及发送
    Sender_StartTimer(TIMER);

    //划分打包塞入windows
    Pack_and_Load();

    //发包
    Sender_Send();
}

/* event handler, called when a packet is passed from the lower layer at the 
   sender */
void Sender_FromLowerLayer(struct packet *pkt)
{
    printf("\t@@@@sender from lower\n");
    //ack也检查一下checksum
    short sum;
    sum = *(short *)(pkt->data);
    if (sum != checksum(pkt) || pkt->data[4] != A_CONST)
        return;

    //返回的seq只含有序号信息
    char ack_seq = pkt->data[3];
    if (ack_seq < 0 || ack_seq >= 60 || pkt->data[2] != 0)
        return;
    // char waiting_seq = pkt->data[4]; //reciever端 等的seq

    //两种无效ack的情况
    //一种是windows已腾空
    if (!n_pkt_window)
    {
        printf("\t***\tget invalid ack 1\n");
        return;
    }
    //一种是ack不在有效范围内
    char first_seq_in_window = windows[window_cursor].seq;
    int difference = ack_seq - first_seq_in_window;

    if ((difference >= -50 && difference < 0) || difference >= 10)
    {
        printf("\t***\tget invalid ack 2\n");
        return;
    }

    printf("@@@sender from low—— get ack seq=%d\n", ack_seq);
    PacketInWindow *p;
    //如果是收到了当前windows中的第一个包的seq,则可以移动windows
    if (ack_seq % MAX_WINDOW_SIZE == window_cursor)
    {
        int i = 0;
        //重置windows中已经发送成功的packet
        while (i < n_pkt_window)
        {
            p = &windows[(window_cursor + i) % MAX_WINDOW_SIZE];
            //确保windows中的packet与自己的位置相符
            assert((p->seq % 10) == (window_cursor + i) % 10);
            //因为cursor对应的包还没设置ack，可以直接free
            if (i == 0 || p->if_ack)
            {
                assert(p->pkt);
                free(p->pkt);
                p->pkt = NULL;
                p->if_ack = false;
                p->seq = 0;
                i++;
                continue;
            }
            // i++;
            break;
        }
        n_pkt_window -= i;
        assert(n_pkt_window >= 0);
        window_cursor = (window_cursor + i) % MAX_WINDOW_SIZE;

        Sender_StartTimer(TIMER);

        Pack_and_Load();
        Sender_Send();
        return;
    }

    //收到其他的packet的ack
    assert(windows[ack_seq % MAX_WINDOW_SIZE].seq % MAX_WINDOW_SIZE == ack_seq % MAX_WINDOW_SIZE);
    windows[ack_seq % MAX_WINDOW_SIZE].if_ack = true;
    //若是最后一个发送的包的ack收到了，且不是windows首位，则重发windows中的第一个包！
    Seq_increase(ack_seq);
    if (ack_seq == next_send_packet_seq)
    {
        // Sender_StopTimer();
        Sender_StartTimer(TIMER);
        Pack_and_Load();
        Sender_Send();
    }
}

/* event handler, called when the timer expires */
void Sender_Timeout()
{
    // printf("\t$$$Timeout! waiting for ack seq=%d, and has %d packets in windows\n", windows[window_cursor].seq, n_pkt_window);
    Sender_StartTimer(TIMER);
    //timer expires
    //将下一个要发的包的序号改为windows中的第一个
    next_send_packet_seq = windows[window_cursor].seq;
    Pack_and_Load();
    Sender_Send();
}
