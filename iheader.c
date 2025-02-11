#include <string.h>
#include "iheader.h"
#include "dnsparser.h"
#include "dnsgenerator.h"
#include "common.h"
#include "logs.h"

static BOOL ap = FALSE;

void IHeader_Reset(IHeader *h)
{
    h->Parent = NULL;
    h->RequestTcp = FALSE;
    h->Agent[0] = '\0';
    h->BackAddress.family = AF_UNSPEC;
    h->Domain[0] = '\0';
    h->HashValue = 0;
    h->EDNSEnabled = FALSE;
}

int IHeader_Fill(IHeader *h,
                 BOOL ReturnHeader, /* For tcp, this will be ignored */
                 char *DnsEntity,
                 int EntityLength,
                 struct sockaddr *BackAddress, /* NULL for tcp */
                 SOCKET SendBackSocket,
                 sa_family_t Family, /* For tcp, this will be ignored */
                 const char *Agent
                 )
{
    DnsSimpleParser p;
    DnsSimpleParserIterator i;

    h->Parent = NULL;
    h->RequestTcp = FALSE;
    h->EDNSEnabled = FALSE;

    if( DnsSimpleParser_Init(&p, DnsEntity, EntityLength, FALSE) != 0 )
    {
        return -31;
    }

    if( DnsSimpleParserIterator_Init(&i, &p) != 0 )
    {
        return -36;
    }

    while( i.Next(&i) != NULL )
    {
        switch( i.Purpose )
        {
        case DNS_RECORD_PURPOSE_QUESTION:
            if( i.Klass != DNS_CLASS_IN )
            {
                return -48;
            }

            if( i.GetName(&i, h->Domain, sizeof(h->Domain)) < 0 )
            {
                return -46;
            }

            StrToLower(h->Domain);
            h->HashValue = HASH(h->Domain, 0);
            h->Type = (DNSRecordType)DNSGetRecordType(DNSJumpHeader(DnsEntity));
            break;

        case DNS_RECORD_PURPOSE_ADDITIONAL:
            if( i.Type == DNS_TYPE_OPT )
            {
                h->EDNSEnabled = TRUE;
            }
            break;

        default:
            break;
        }
    }

    h->ReturnHeader = ReturnHeader;

    if( BackAddress != NULL )
    {
        memcpy(&(h->BackAddress.Addr), BackAddress, GetAddressLength(Family));
        h->BackAddress.family = Family;
    } else {
        h->BackAddress.family = AF_UNSPEC;
    }

    h->SendBackSocket = SendBackSocket;

    if( Agent != NULL )
    {
        strncpy(h->Agent, Agent, sizeof(h->Agent));
        h->Agent[sizeof(h->Agent) - 1] = '\0';
    } else {
        h->Agent[0] = '\0';
    }

    h->EntityLength = EntityLength;

    return 0;
}


int MsgContext_Init(BOOL _ap)
{
    ap = _ap;

    return 0;
}

int MsgContext_AddFakeEdns(MsgContext *MsgCtx, int BufferLength)
{
    DnsGenerator g;
    IHeader *h = (IHeader *)MsgCtx;

    if( ap == FALSE || h->EDNSEnabled )
    {
        return 0;
    }

    if( DnsGenerator_Init(&g,
                          IHEADER_TAIL(h),
                          BufferLength - sizeof(IHeader),
                          IHEADER_TAIL(h),
                          h->EntityLength,
                          FALSE
                          )
        != 0 )
    {
        return -125;
    }

    while( g.NextPurpose(&g) != DNS_RECORD_PURPOSE_ADDITIONAL );

    g.EDns(&g, 1280);

    h->EntityLength = g.Length(&g);
    h->EDNSEnabled = TRUE;

    return 0;
}

BOOL MsgContext_IsBlocked(MsgContext *MsgCtx)
{
    IHeader *h = (IHeader *)MsgCtx;

    return (ap && !(h->EDNSEnabled));
}

BOOL MsgContext_IsFromTCP(MsgContext *MsgCtx)
{
    IHeader *h = (IHeader *)MsgCtx;

    return (h->BackAddress.family == AF_UNSPEC);
}

int MsgContext_SendBack(MsgContext *MsgCtx)
{
    IHeader *h = (IHeader *)MsgCtx;
    char *Content = (char *)(IHEADER_TAIL(h));
    int Length = h->EntityLength;

    if( MsgContext_IsFromTCP(MsgCtx) )
    {
        /* TCP */
        Content -= 2;
        Length += 2;

        DNSSetTcpLength(Content, h->EntityLength);

        if( send(h->SendBackSocket,
                 Content,
                 Length,
                 MSG_NOSIGNAL
                 )
            != Length )
        {
            /** TODO: Show error */
            return -112;
        }
    } else {
        /* UDP */
        if( h->ReturnHeader )
        {
            Content -= sizeof(IHeader);
            Length += sizeof(IHeader);
        }

        if( sendto(h->SendBackSocket,
                   Content,
                   Length,
                   MSG_NOSIGNAL,
                   (const struct sockaddr *)&(h->BackAddress.Addr),
                   GetAddressLength(h->BackAddress.family)
                   )
           != Length )
        {
            /** TODO: Show error */
            return -138;
        }
    }

    return 0;
}

int MsgContext_SendBackRefusedMessage(MsgContext *MsgCtx)
{
    IHeader *h = (IHeader *)MsgCtx;
    DNSHeader *RequestContent = IHEADER_TAIL(h);

    RequestContent->Flags.Direction = 1;
    RequestContent->Flags.RecursionAvailable = 1;
    RequestContent->Flags.ResponseCode = 0;

    return MsgContext_SendBack(MsgCtx);

}
