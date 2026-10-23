/* Copyright (C) 2026 NOS contributors. Actual freestanding NLP core in user mode. */
#include "abi.h"
#include "nlp/capabilities.h"
#include "nlp/hash.h"
#include "nlp/router.h"
#include <string.h>
void nos_check(int, const char *);
static unsigned received;
static int packet(void *context, const uint8_t *raw, size_t size)
{
    struct nlp_frame f;
    (void)context;
    if (nlp_frame_decode(raw, size, &f) || f.length != 3 || memcmp(f.payload, "NLP", 3)) return -NLP_INVALID;
    ++received; return 0;
}
static void wire_transport(void)
{
    struct nlp_header h = {.kind=NLP_EVENT,.priority=1,.hop_limit=16,.fabric_epoch=1,
        .src_node=1,.dst_node=2,.session_id=1,.packet_counter=1};
    struct nlp_frame f;struct nlp_stream stream;
    uint8_t raw[NLP_PDU_MAX],framed[NLP_ENCODED_MAX];size_t size,wrapped,consumed;
    int good=!nlp_frame_encode(&h,"NLP",3,raw,sizeof(raw),&size);
    good=good&&!nlp_frame_decode(raw,size,&f)&&f.length==3&&nlp_crc32c("123456789",9)==0xe3069283;
    raw[size-1]^=1;good=good&&nlp_frame_decode(raw,size,&f)==-NLP_INTEGRITY;raw[size-1]^=1;
    nos_check(good,"nlp_wire_crc_bounded_decode");
    good=1;received=0;
    for(unsigned mode=0;mode<2;++mode){
        nlp_stream_init(&stream,mode);good=good&&!nlp_stream_wrap(mode,raw,size,framed,sizeof(framed),&wrapped);
        for(size_t i=0;i<wrapped;++i)good=good&&!nlp_stream_feed(&stream,framed+i,1,&consumed,0,packet,NULL)&&consumed==1;
    }
    nos_check(good&&received==2,"nlp_uart_tcp_actual_fragmented_frames");
}
static void can_transport(void)
{
    struct nlp_can can={0};struct nlp_header h={.kind=3,.priority=1,.hop_limit=16,.fabric_epoch=1,
        .src_node=1,.dst_node=2,.session_id=1,.packet_counter=1};
    uint8_t raw[NLP_PDU_MAX],fragment[64];size_t size,written;uint32_t id;
    int good=!nlp_frame_encode(&h,"NLP",3,raw,sizeof(raw),&size);
    nlp_can_reset(&can,2);received=0;good=good&&!nlp_can_id(1,1,2,0,0,&id);
    for(unsigned index=0;good;++index){
        good=good&&!nlp_can_fragment(raw,size,8,index,fragment,&written);
        if(good)good=good&&!nlp_can_receive(&can,id,fragment,written,index,packet,NULL);
        if(fragment[1]&2)break;
    }
    nos_check(good&&received==1,"nlp_can_classic_reassembly");
}
static void route_and_hash(void)
{
    struct nlp_router router;uint8_t uid[16]={1},fabric[16]={2},child[16]={3},digest[32];
    static const uint8_t expected[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    nos_check(!nlp_sha256("abc",3,digest)&&!memcmp(digest,expected,32),"nlp_freestanding_sha256");
    nlp_router_init(&router,uid);
    int good=!nlp_router_join(&router,fabric,1,1,NLP_NO_PORT,NULL,0)&&
        !nlp_router_prepare(&router,2,child,0,1,0,1000)&&!nlp_router_commit(&router,1);
    struct nlp_header h={.kind=3,.priority=1,.hop_limit=16,.fabric_epoch=1,.src_node=1,.dst_node=2,.session_id=1,.packet_counter=1};
    uint8_t raw[NLP_PDU_MAX],egress;size_t size;
    good=good&&!nlp_frame_encode(&h,NULL,0,raw,sizeof(raw),&size)&&!nlp_router_forward(&router,NLP_NO_PORT,raw,size,&egress)&&!egress&&raw[7]==16;
    nlp_router_expire(&router,1000);
    good=good&&nlp_router_forward(&router,NLP_NO_PORT,raw,size,&egress)==-NLP_NO_ROUTE;
    nos_check(good,"nlp_tree_commit_forward_and_expiry");
}
void nos_nlp_regression(void)
{
    wire_transport();can_transport();route_and_hash();
}
