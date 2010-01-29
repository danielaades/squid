
/*
 * $Id$
 *
 * DEBUG: section 85    Client-side Request Routines
 * AUTHOR: Robert Collins (Originally Duane Wessels in client_side.c)
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 */


/*
 * General logic of request processing:
 *
 * We run a series of tests to determine if access will be permitted, and to do
 * any redirection. Then we call into the result clientStream to retrieve data.
 * From that point on it's up to reply management.
 */

#include "squid.h"
#include "clientStream.h"
#include "client_side_request.h"
#include "auth/UserRequest.h"
#include "HttpRequest.h"
#include "ProtoPort.h"
#include "acl/FilledChecklist.h"
#include "acl/Gadgets.h"
#include "client_side.h"
#include "client_side_reply.h"
#include "Store.h"
#include "HttpReply.h"
#include "MemObject.h"
#include "ClientRequestContext.h"
#include "SquidTime.h"
#include "wordlist.h"
#include "inet_pton.h"
#include "fde.h"

#if USE_ADAPTATION
#include "adaptation/AccessCheck.h"
#include "adaptation/Iterator.h"
#include "adaptation/Service.h"
#if ICAP_CLIENT
#include "adaptation/icap/History.h"
#endif
//static void adaptationAclCheckDoneWrapper(Adaptation::ServicePointer service, void *data);
#endif



#if LINGERING_CLOSE
#define comm_close comm_lingering_close
#endif

static const char *const crlf = "\r\n";

#if FOLLOW_X_FORWARDED_FOR
static void
clientFollowXForwardedForCheck(int answer, void *data);
#endif /* FOLLOW_X_FORWARDED_FOR */

CBDATA_CLASS_INIT(ClientRequestContext);

void *
ClientRequestContext::operator new (size_t size)
{
    assert (size == sizeof(ClientRequestContext));
    CBDATA_INIT_TYPE(ClientRequestContext);
    ClientRequestContext *result = cbdataAlloc(ClientRequestContext);
    return result;
}

void
ClientRequestContext::operator delete (void *address)
{
    ClientRequestContext *t = static_cast<ClientRequestContext *>(address);
    cbdataFree(t);
}

/* Local functions */
/* other */
static void clientAccessCheckDoneWrapper(int, void *);
static int clientHierarchical(ClientHttpRequest * http);
static void clientInterpretRequestHeaders(ClientHttpRequest * http);
static RH clientRedirectDoneWrapper;
static PF checkNoCacheDoneWrapper;
extern "C" CSR clientGetMoreData;
extern "C" CSS clientReplyStatus;
extern "C" CSD clientReplyDetach;
static void checkFailureRatio(err_type, hier_code);

ClientRequestContext::~ClientRequestContext()
{
    /*
     * Release our "lock" on our parent, ClientHttpRequest, if we
     * still have one
     */

    if (http)
        cbdataReferenceDone(http);

    debugs(85,3, HERE << this << " ClientRequestContext destructed");
}

ClientRequestContext::ClientRequestContext(ClientHttpRequest *anHttp) : http(cbdataReference(anHttp)), acl_checklist (NULL), redirect_state (REDIRECT_NONE)
{
    http_access_done = false;
    redirect_done = false;
    no_cache_done = false;
    interpreted_req_hdrs = false;
    debugs(85,3, HERE << this << " ClientRequestContext constructed");
}

CBDATA_CLASS_INIT(ClientHttpRequest);

void *
ClientHttpRequest::operator new (size_t size)
{
    assert (size == sizeof (ClientHttpRequest));
    CBDATA_INIT_TYPE(ClientHttpRequest);
    ClientHttpRequest *result = cbdataAlloc(ClientHttpRequest);
    return result;
}

void
ClientHttpRequest::operator delete (void *address)
{
    ClientHttpRequest *t = static_cast<ClientHttpRequest *>(address);
    cbdataFree(t);
}

ClientHttpRequest::ClientHttpRequest(ConnStateData * aConn) :
#if USE_ADAPTATION
        AsyncJob("ClientHttpRequest"),
#endif
        loggingEntry_(NULL)
{
    start_time = current_time;
    setConn(aConn);
    dlinkAdd(this, &active, &ClientActiveRequests);
#if USE_ADAPTATION
    request_satisfaction_mode = false;
#endif
}

/*
 * returns true if client specified that the object must come from the cache
 * without contacting origin server
 */
bool
ClientHttpRequest::onlyIfCached()const
{
    assert(request);
    return request->cache_control &&
           EBIT_TEST(request->cache_control->mask, CC_ONLY_IF_CACHED);
}

/*
 * This function is designed to serve a fairly specific purpose.
 * Occasionally our vBNS-connected caches can talk to each other, but not
 * the rest of the world.  Here we try to detect frequent failures which
 * make the cache unusable (e.g. DNS lookup and connect() failures).  If
 * the failure:success ratio goes above 1.0 then we go into "hit only"
 * mode where we only return UDP_HIT or UDP_MISS_NOFETCH.  Neighbors
 * will only fetch HITs from us if they are using the ICP protocol.  We
 * stay in this mode for 5 minutes.
 *
 * Duane W., Sept 16, 1996
 */

#define FAILURE_MODE_TIME 300

static void
checkFailureRatio(err_type etype, hier_code hcode)
{
    static double magic_factor = 100.0;
    double n_good;
    double n_bad;

    if (hcode == HIER_NONE)
        return;

    n_good = magic_factor / (1.0 + request_failure_ratio);

    n_bad = magic_factor - n_good;

    switch (etype) {

    case ERR_DNS_FAIL:

    case ERR_CONNECT_FAIL:
    case ERR_SECURE_CONNECT_FAIL:

    case ERR_READ_ERROR:
        n_bad++;
        break;

    default:
        n_good++;
    }

    request_failure_ratio = n_bad / n_good;

    if (hit_only_mode_until > squid_curtime)
        return;

    if (request_failure_ratio < 1.0)
        return;

    debugs(33, 0, "Failure Ratio at "<< std::setw(4)<<
           std::setprecision(3) << request_failure_ratio);

    debugs(33, 0, "Going into hit-only-mode for " <<
           FAILURE_MODE_TIME / 60 << " minutes...");

    hit_only_mode_until = squid_curtime + FAILURE_MODE_TIME;

    request_failure_ratio = 0.8;	/* reset to something less than 1.0 */
}

ClientHttpRequest::~ClientHttpRequest()
{
    debugs(33, 3, "httpRequestFree: " << uri);
    PROF_start(httpRequestFree);

    // Even though freeResources() below may destroy the request,
    // we no longer set request->body_pipe to NULL here
    // because we did not initiate that pipe (ConnStateData did)

    /* the ICP check here was erroneous
     * - StoreEntry::releaseRequest was always called if entry was valid
     */
    assert(logType < LOG_TYPE_MAX);

    logRequest();

    loggingEntry(NULL);

    if (request)
        checkFailureRatio(request->errType, al.hier.code);

    freeResources();

#if USE_ADAPTATION
    announceInitiatorAbort(virginHeadSource);

    if (adaptedBodySource != NULL)
        stopConsumingFrom(adaptedBodySource);
#endif

    if (calloutContext)
        delete calloutContext;

    if (conn_)
        cbdataReferenceDone(conn_);

    /* moving to the next connection is handled by the context free */
    dlinkDelete(&active, &ClientActiveRequests);

    PROF_stop(httpRequestFree);
}

/* Create a request and kick it off */
/*
 * TODO: Pass in the buffers to be used in the inital Read request, as they are
 * determined by the user
 */
int				/* returns nonzero on failure */
clientBeginRequest(const HttpRequestMethod& method, char const *url, CSCB * streamcallback,
                   CSD * streamdetach, ClientStreamData streamdata, HttpHeader const *header,
                   char *tailbuf, size_t taillen)
{
    size_t url_sz;
    HttpVersion http_ver (1, 0);
    ClientHttpRequest *http = new ClientHttpRequest(NULL);
    HttpRequest *request;
    StoreIOBuffer tempBuffer;
    http->start_time = current_time;
    /* this is only used to adjust the connection offset in client_side.c */
    http->req_sz = 0;
    tempBuffer.length = taillen;
    tempBuffer.data = tailbuf;
    /* client stream setup */
    clientStreamInit(&http->client_stream, clientGetMoreData, clientReplyDetach,
                     clientReplyStatus, new clientReplyContext(http), streamcallback,
                     streamdetach, streamdata, tempBuffer);
    /* make it visible in the 'current acctive requests list' */
    /* Set flags */
    /* internal requests only makes sense in an
     * accelerator today. TODO: accept flags ? */
    http->flags.accel = 1;
    /* allow size for url rewriting */
    url_sz = strlen(url) + Config.appendDomainLen + 5;
    http->uri = (char *)xcalloc(url_sz, 1);
    strcpy(http->uri, url);

    if ((request = HttpRequest::CreateFromUrlAndMethod(http->uri, method)) == NULL) {
        debugs(85, 5, "Invalid URL: " << http->uri);
        return -1;
    }

    /*
     * now update the headers in request with our supplied headers. urLParse
     * should return a blank header set, but we use Update to be sure of
     * correctness.
     */
    if (header)
        request->header.update(header, NULL);

    http->log_uri = xstrdup(urlCanonicalClean(request));

    /* http struct now ready */

    /*
     * build new header list *? TODO
     */
    request->flags.accelerated = http->flags.accel;

    request->flags.internalclient = 1;

    /* this is an internally created
     * request, not subject to acceleration
     * target overrides */
    /*
     * FIXME? Do we want to detect and handle internal requests of internal
     * objects ?
     */

    /* Internally created requests cannot have bodies today */
    request->content_length = 0;

    request->client_addr.SetNoAddr();

#if FOLLOW_X_FORWARDED_FOR
    request->indirect_client_addr.SetNoAddr();
#endif /* FOLLOW_X_FORWARDED_FOR */

    request->my_addr.SetNoAddr();	/* undefined for internal requests */

    request->my_addr.SetPort(0);

    request->http_ver = http_ver;

    http->request = HTTPMSGLOCK(request);

    /* optional - skip the access check ? */
    http->calloutContext = new ClientRequestContext(http);

    http->calloutContext->http_access_done = false;

    http->calloutContext->redirect_done = true;

    http->calloutContext->no_cache_done = true;

    http->doCallouts();

    return 0;
}

bool
ClientRequestContext::httpStateIsValid()
{
    ClientHttpRequest *http_ = http;

    if (cbdataReferenceValid(http_))
        return true;

    http = NULL;

    cbdataReferenceDone(http_);

    return false;
}

#if FOLLOW_X_FORWARDED_FOR
/**
 * clientFollowXForwardedForCheck() checks the content of X-Forwarded-For:
 * against the followXFF ACL, or cleans up and passes control to
 * clientAccessCheck().
 *
 * The trust model here is a little ambiguous. So to clarify the logic:
 * - we may always use the direct client address as the client IP.
 * - these trust tests merey tell whether we trust given IP enough to believe the
 *   IP string which it appended to the X-Forwarded-For: header.
 * - if at any point we don't trust what an IP adds we stop looking.
 * - at that point the current contents of indirect_client_addr are the value set
 *   by the last previously trusted IP.
 * ++ indirect_client_addr contains the remote direct client from the trusted peers viewpoint.
 */
static void
clientFollowXForwardedForCheck(int answer, void *data)
{
    ClientRequestContext *calloutContext = (ClientRequestContext *) data;

    if (!calloutContext->httpStateIsValid())
        return;

    ClientHttpRequest *http = calloutContext->http;
    HttpRequest *request = http->request;

    /*
     * answer should be be ACCESS_ALLOWED or ACCESS_DENIED if we are
     * called as a result of ACL checks, or -1 if we are called when
     * there's nothing left to do.
     */
    if (answer == ACCESS_ALLOWED &&
            request->x_forwarded_for_iterator.size () != 0) {

        /*
         * Remove the last comma-delimited element from the
         * x_forwarded_for_iterator and use it to repeat the cycle.
         */
        const char *p;
        const char *asciiaddr;
        int l;
        IpAddress addr;
        p = request->x_forwarded_for_iterator.termedBuf();
        l = request->x_forwarded_for_iterator.size();

        /*
        * XXX x_forwarded_for_iterator should really be a list of
        * IP addresses, but it's a String instead.  We have to
        * walk backwards through the String, biting off the last
        * comma-delimited part each time.  As long as the data is in
        * a String, we should probably implement and use a variant of
        * strListGetItem() that walks backwards instead of forwards
        * through a comma-separated list.  But we don't even do that;
        * we just do the work in-line here.
        */
        /* skip trailing space and commas */
        while (l > 0 && (p[l-1] == ',' || xisspace(p[l-1])))
            l--;
        request->x_forwarded_for_iterator.cut(l);
        /* look for start of last item in list */
        while (l > 0 && ! (p[l-1] == ',' || xisspace(p[l-1])))
            l--;
        asciiaddr = p+l;
        if ((addr = asciiaddr)) {
            request->indirect_client_addr = addr;
            request->x_forwarded_for_iterator.cut(l);
            calloutContext->acl_checklist = clientAclChecklistCreate(Config.accessList.followXFF, http);
            if (!Config.onoff.acl_uses_indirect_client) {
                /* override the default src_addr tested if we have to go deeper than one level into XFF */
                Filled(calloutContext->acl_checklist)->src_addr = request->indirect_client_addr;
            }
            calloutContext->acl_checklist->nonBlockingCheck(clientFollowXForwardedForCheck, data);
            return;
        }
    } /*if (answer == ACCESS_ALLOWED &&
        request->x_forwarded_for_iterator.size () != 0)*/

    /* clean up, and pass control to clientAccessCheck */
    if (Config.onoff.log_uses_indirect_client) {
        /*
        * Ensure that the access log shows the indirect client
        * instead of the direct client.
        */
        ConnStateData *conn = http->getConn();
        conn->log_addr = request->indirect_client_addr;
    }
    request->x_forwarded_for_iterator.clean();
    request->flags.done_follow_x_forwarded_for = 1;

    if (answer != ACCESS_ALLOWED && answer != ACCESS_DENIED) {
        debugs(28, DBG_CRITICAL, "ERROR: Processing X-Forwarded-For. Stopping at IP address: " << request->indirect_client_addr );
    }

    /* process actual access ACL as normal. */
    calloutContext->clientAccessCheck();
}
#endif /* FOLLOW_X_FORWARDED_FOR */

/* This is the entry point for external users of the client_side routines */
void
ClientRequestContext::clientAccessCheck()
{
#if FOLLOW_X_FORWARDED_FOR
    if (!http->request->flags.done_follow_x_forwarded_for &&
            Config.accessList.followXFF &&
            http->request->header.has(HDR_X_FORWARDED_FOR)) {

        /* we always trust the direct client address for actual use */
        http->request->indirect_client_addr = http->request->client_addr;
        http->request->indirect_client_addr.SetPort(0);

        /* setup the XFF iterator for processing */
        http->request->x_forwarded_for_iterator = http->request->header.getList(HDR_X_FORWARDED_FOR);

        /* begin by checking to see if we trust direct client enough to walk XFF */
        acl_checklist = clientAclChecklistCreate(Config.accessList.followXFF, http);
        acl_checklist->nonBlockingCheck(clientFollowXForwardedForCheck, this);
        return;
    }
#endif /* FOLLOW_X_FORWARDED_FOR */

    if (Config.accessList.http) {
        acl_checklist = clientAclChecklistCreate(Config.accessList.http, http);
        acl_checklist->nonBlockingCheck(clientAccessCheckDoneWrapper, this);
    } else {
        debugs(0, DBG_CRITICAL, "No http_access configuration found. This will block ALL traffic");
        clientAccessCheckDone(ACCESS_DENIED);
    }
}

void
clientAccessCheckDoneWrapper(int answer, void *data)
{
    ClientRequestContext *calloutContext = (ClientRequestContext *) data;

    if (!calloutContext->httpStateIsValid())
        return;

    calloutContext->clientAccessCheckDone(answer);
}

void
ClientRequestContext::clientAccessCheckDone(int answer)
{
    acl_checklist = NULL;
    err_type page_id;
    http_status status;
    debugs(85, 2, "The request " <<
           RequestMethodStr(http->request->method) << " " <<
           http->uri << " is " <<
           (answer == ACCESS_ALLOWED ? "ALLOWED" : "DENIED") <<
           ", because it matched '" <<
           (AclMatchedName ? AclMatchedName : "NO ACL's") << "'" );
    char const *proxy_auth_msg = "<null>";

    if (http->getConn() != NULL && http->getConn()->auth_user_request != NULL)
        proxy_auth_msg = http->getConn()->auth_user_request->denyMessage("<null>");
    else if (http->request->auth_user_request != NULL)
        proxy_auth_msg = http->request->auth_user_request->denyMessage("<null>");

    if (answer != ACCESS_ALLOWED) {
        /* Send an error */
        int require_auth = (answer == ACCESS_REQ_PROXY_AUTH || aclIsProxyAuth(AclMatchedName));
        debugs(85, 5, "Access Denied: " << http->uri);
        debugs(85, 5, "AclMatchedName = " << (AclMatchedName ? AclMatchedName : "<null>"));

        if (require_auth)
            debugs(33, 5, "Proxy Auth Message = " << (proxy_auth_msg ? proxy_auth_msg : "<null>"));

        /*
         * NOTE: get page_id here, based on AclMatchedName because if
         * USE_DELAY_POOLS is enabled, then AclMatchedName gets clobbered in
         * the clientCreateStoreEntry() call just below.  Pedro Ribeiro
         * <pribeiro@isel.pt>
         */
        page_id = aclGetDenyInfoPage(&Config.denyInfoList, AclMatchedName, answer != ACCESS_REQ_PROXY_AUTH);

        http->logType = LOG_TCP_DENIED;

        if (require_auth) {
            if (!http->flags.accel) {
                /* Proxy authorisation needed */
                status = HTTP_PROXY_AUTHENTICATION_REQUIRED;
            } else {
                /* WWW authorisation needed */
                status = HTTP_UNAUTHORIZED;
            }

            if (page_id == ERR_NONE)
                page_id = ERR_CACHE_ACCESS_DENIED;
        } else {
            status = HTTP_FORBIDDEN;

            if (page_id == ERR_NONE)
                page_id = ERR_ACCESS_DENIED;
        }

        clientStreamNode *node = (clientStreamNode *)http->client_stream.tail->prev->data;
        clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
        assert (repContext);
        IpAddress tmpnoaddr;
        tmpnoaddr.SetNoAddr();
        repContext->setReplyToError(page_id, status,
                                    http->request->method, NULL,
                                    http->getConn() != NULL ? http->getConn()->peer : tmpnoaddr,
                                    http->request,
                                    NULL,
                                    http->getConn() != NULL && http->getConn()->auth_user_request ?
                                    http->getConn()->auth_user_request : http->request->auth_user_request);

        node = (clientStreamNode *)http->client_stream.tail->data;
        clientStreamRead(node, http, node->readBuffer);
        return;
    }

    /* ACCESS_ALLOWED continues here ... */
    safe_free(http->uri);

    http->uri = xstrdup(urlCanonical(http->request));

    http->doCallouts();
}

#if USE_ADAPTATION
static void
adaptationAclCheckDoneWrapper(Adaptation::ServiceGroupPointer g, void *data)
{
    ClientRequestContext *calloutContext = (ClientRequestContext *)data;

    if (!calloutContext->httpStateIsValid())
        return;

    calloutContext->adaptationAclCheckDone(g);
}

void
ClientRequestContext::adaptationAclCheckDone(Adaptation::ServiceGroupPointer g)
{
    debugs(93,3,HERE << this << " adaptationAclCheckDone called");
    assert(http);

#if ICAP_CLIENT
    Adaptation::Icap::History::Pointer ih = http->request->icapHistory();
    if (ih != NULL) {
        if (http->getConn() != NULL) {
            ih->rfc931 = http->getConn()->rfc931;
#if USE_SSL
            ih->ssluser = sslGetUserEmail(fd_table[http->getConn()->fd].ssl);
#endif
        }
        ih->log_uri = http->log_uri;
        ih->req_sz = http->req_sz;
    }
#endif

    if (!g) {
        debugs(85,3, HERE << "no adaptation needed");
        http->doCallouts();
        return;
    }

    http->startAdaptation(g);
}

#endif

static void
clientRedirectAccessCheckDone(int answer, void *data)
{
    ClientRequestContext *context = (ClientRequestContext *)data;
    ClientHttpRequest *http = context->http;
    context->acl_checklist = NULL;

    if (answer == ACCESS_ALLOWED)
        redirectStart(http, clientRedirectDoneWrapper, context);
    else
        context->clientRedirectDone(NULL);
}

void
ClientRequestContext::clientRedirectStart()
{
    debugs(33, 5, "clientRedirectStart: '" << http->uri << "'");

    if (Config.accessList.redirector) {
        acl_checklist = clientAclChecklistCreate(Config.accessList.redirector, http);
        acl_checklist->nonBlockingCheck(clientRedirectAccessCheckDone, this);
    } else
        redirectStart(http, clientRedirectDoneWrapper, this);
}

static int
clientHierarchical(ClientHttpRequest * http)
{
    const char *url = http->uri;
    HttpRequest *request = http->request;
    HttpRequestMethod method = request->method;
    const wordlist *p = NULL;

    /*
     * IMS needs a private key, so we can use the hierarchy for IMS only if our
     * neighbors support private keys
     */

    if (request->flags.ims && !neighbors_do_private_keys)
        return 0;

    /*
     * This is incorrect: authenticating requests can be sent via a hierarchy
     * (they can even be cached if the correct headers are set on the reply)
     */
    if (request->flags.auth)
        return 0;

    if (method == METHOD_TRACE)
        return 1;

    if (method != METHOD_GET)
        return 0;

    /* scan hierarchy_stoplist */
    for (p = Config.hierarchy_stoplist; p; p = p->next)
        if (strstr(url, p->key))
            return 0;

    if (request->flags.loopdetect)
        return 0;

    if (request->protocol == PROTO_HTTP)
        return httpCachable(method);

    if (request->protocol == PROTO_GOPHER)
        return gopherCachable(request);

    if (request->protocol == PROTO_CACHEOBJ)
        return 0;

    return 1;
}


static void
clientCheckPinning(ClientHttpRequest * http)
{
    HttpRequest *request = http->request;
    HttpHeader *req_hdr = &request->header;
    ConnStateData *http_conn = http->getConn();

    /* Internal requests such as those from ESI includes may be without
     * a client connection
     */
    if (!http_conn)
        return;

    request->flags.connection_auth_disabled = http_conn->port->connection_auth_disabled;
    if (!request->flags.connection_auth_disabled) {
        if (http_conn->pinning.fd != -1) {
            if (http_conn->pinning.auth) {
                request->flags.connection_auth = 1;
                request->flags.auth = 1;
            } else {
                request->flags.connection_proxy_auth = 1;
            }
            request->setPinnedConnection(http_conn);
        }
    }

    /* check if connection auth is used, and flag as candidate for pinning
     * in such case.
     * Note: we may need to set flags.connection_auth even if the connection
     * is already pinned if it was pinned earlier due to proxy auth
     */
    if (!request->flags.connection_auth) {
        if (req_hdr->has(HDR_AUTHORIZATION) || req_hdr->has(HDR_PROXY_AUTHORIZATION)) {
            HttpHeaderPos pos = HttpHeaderInitPos;
            HttpHeaderEntry *e;
            int may_pin = 0;
            while ((e = req_hdr->getEntry(&pos))) {
                if (e->id == HDR_AUTHORIZATION || e->id == HDR_PROXY_AUTHORIZATION) {
                    const char *value = e->value.rawBuf();
                    if (strncasecmp(value, "NTLM ", 5) == 0
                            ||
                            strncasecmp(value, "Negotiate ", 10) == 0
                            ||
                            strncasecmp(value, "Kerberos ", 9) == 0) {
                        if (e->id == HDR_AUTHORIZATION) {
                            request->flags.connection_auth = 1;
                            may_pin = 1;
                        } else {
                            request->flags.connection_proxy_auth = 1;
                            may_pin = 1;
                        }
                    }
                }
            }
            if (may_pin && !request->pinnedConnection()) {
                request->setPinnedConnection(http->getConn());
            }
        }
    }
}

static void
clientInterpretRequestHeaders(ClientHttpRequest * http)
{
    HttpRequest *request = http->request;
    HttpHeader *req_hdr = &request->header;
    int no_cache = 0;
    const char *str;

    request->imslen = -1;
    request->ims = req_hdr->getTime(HDR_IF_MODIFIED_SINCE);

    if (request->ims > 0)
        request->flags.ims = 1;

    if (!request->flags.ignore_cc) {
        if (req_hdr->has(HDR_PRAGMA)) {
            String s = req_hdr->getList(HDR_PRAGMA);

            if (strListIsMember(&s, "no-cache", ','))
                no_cache++;

            s.clean();
        }

        if (request->cache_control)
            if (EBIT_TEST(request->cache_control->mask, CC_NO_CACHE))
                no_cache++;

        /*
        * Work around for supporting the Reload button in IE browsers when Squid
        * is used as an accelerator or transparent proxy, by turning accelerated
        * IMS request to no-cache requests. Now knows about IE 5.5 fix (is
        * actually only fixed in SP1, but we can't tell whether we are talking to
        * SP1 or not so all 5.5 versions are treated 'normally').
        */
        if (Config.onoff.ie_refresh) {
            if (http->flags.accel && request->flags.ims) {
                if ((str = req_hdr->getStr(HDR_USER_AGENT))) {
                    if (strstr(str, "MSIE 5.01") != NULL)
                        no_cache++;
                    else if (strstr(str, "MSIE 5.0") != NULL)
                        no_cache++;
                    else if (strstr(str, "MSIE 4.") != NULL)
                        no_cache++;
                    else if (strstr(str, "MSIE 3.") != NULL)
                        no_cache++;
                }
            }
        }
    }

    if (request->method == METHOD_OTHER) {
        no_cache++;
    }

    if (no_cache) {
#if HTTP_VIOLATIONS

        if (Config.onoff.reload_into_ims)
            request->flags.nocache_hack = 1;
        else if (refresh_nocache_hack)
            request->flags.nocache_hack = 1;
        else
#endif

            request->flags.nocache = 1;
    }

    /* ignore range header in non-GETs or non-HEADs */
    if (request->method == METHOD_GET || request->method == METHOD_HEAD) {
        request->range = req_hdr->getRange();

        if (request->range) {
            request->flags.range = 1;
            clientStreamNode *node = (clientStreamNode *)http->client_stream.tail->data;
            /* XXX: This is suboptimal. We should give the stream the range set,
             * and thereby let the top of the stream set the offset when the
             * size becomes known. As it is, we will end up requesting from 0
             * for evey -X range specification.
             * RBC - this may be somewhat wrong. We should probably set the range
             * iter up at this point.
             */
            node->readBuffer.offset = request->range->lowestOffset(0);
            http->range_iter.pos = request->range->begin();
            http->range_iter.valid = true;
        }
    }

    /* Only HEAD and GET requests permit a Range or Request-Range header.
     * If these headers appear on any other type of request, delete them now.
     */
    else {
        req_hdr->delById(HDR_RANGE);
        req_hdr->delById(HDR_REQUEST_RANGE);
        request->range = NULL;
    }

    if (req_hdr->has(HDR_AUTHORIZATION))
        request->flags.auth = 1;

    clientCheckPinning(http);

    if (request->login[0] != '\0')
        request->flags.auth = 1;

    if (req_hdr->has(HDR_VIA)) {
        String s = req_hdr->getList(HDR_VIA);
        /*
         * ThisCache cannot be a member of Via header, "1.0 ThisCache" can.
         * Note ThisCache2 has a space prepended to the hostname so we don't
         * accidentally match super-domains.
         */

        if (strListIsSubstr(&s, ThisCache2, ',')) {
            debugObj(33, 1, "WARNING: Forwarding loop detected for:\n",
                     request, (ObjPackMethod) & httpRequestPack);
            request->flags.loopdetect = 1;
        }

#if FORW_VIA_DB
        fvdbCountVia(s.termedBuf());

#endif

        s.clean();
    }

    /**
     \todo  --enable-useragent-log and --enable-referer-log. We should
            probably drop those two as the custom log formats accomplish pretty much the same thing..
    */
#if USE_USERAGENT_LOG
    if ((str = req_hdr->getStr(HDR_USER_AGENT)))
        logUserAgent(fqdnFromAddr(http->getConn()->log_addr), str);

#endif
#if USE_REFERER_LOG

    if ((str = req_hdr->getStr(HDR_REFERER)))
        logReferer(fqdnFromAddr(http->getConn()->log_addr), str, http->log_uri);

#endif
#if FORW_VIA_DB

    if (req_hdr->has(HDR_X_FORWARDED_FOR)) {
        String s = req_hdr->getList(HDR_X_FORWARDED_FOR);
        fvdbCountForw(s.termedBuf());
        s.clean();
    }

#endif
    if (request->method == METHOD_TRACE || request->method == METHOD_OPTIONS) {
        request->max_forwards = req_hdr->getInt64(HDR_MAX_FORWARDS);
    }

    request->flags.cachable = http->request->cacheable();

    if (clientHierarchical(http))
        request->flags.hierarchical = 1;

    debugs(85, 5, "clientInterpretRequestHeaders: REQ_NOCACHE = " <<
           (request->flags.nocache ? "SET" : "NOT SET"));
    debugs(85, 5, "clientInterpretRequestHeaders: REQ_CACHABLE = " <<
           (request->flags.cachable ? "SET" : "NOT SET"));
    debugs(85, 5, "clientInterpretRequestHeaders: REQ_HIERARCHICAL = " <<
           (request->flags.hierarchical ? "SET" : "NOT SET"));

}

void
clientRedirectDoneWrapper(void *data, char *result)
{
    ClientRequestContext *calloutContext = (ClientRequestContext *)data;

    if (!calloutContext->httpStateIsValid())
        return;

    calloutContext->clientRedirectDone(result);
}

void
ClientRequestContext::clientRedirectDone(char *result)
{
    HttpRequest *new_request = NULL;
    HttpRequest *old_request = http->request;
    debugs(85, 5, "clientRedirectDone: '" << http->uri << "' result=" << (result ? result : "NULL"));
    assert(redirect_state == REDIRECT_PENDING);
    redirect_state = REDIRECT_DONE;

    if (result) {
        http_status status = (http_status) atoi(result);

        if (status == HTTP_MOVED_PERMANENTLY
                || status == HTTP_MOVED_TEMPORARILY
                || status == HTTP_SEE_OTHER
                || status == HTTP_TEMPORARY_REDIRECT) {
            char *t = result;

            if ((t = strchr(result, ':')) != NULL) {
                http->redirect.status = status;
                http->redirect.location = xstrdup(t + 1);
            } else {
                debugs(85, 1, "clientRedirectDone: bad input: " << result);
            }
        } else if (strcmp(result, http->uri))
            new_request = HttpRequest::CreateFromUrlAndMethod(result, old_request->method);
    }

    if (new_request) {
        safe_free(http->uri);
        http->uri = xstrdup(urlCanonical(new_request));
        new_request->http_ver = old_request->http_ver;
        new_request->header.append(&old_request->header);
        new_request->client_addr = old_request->client_addr;
#if FOLLOW_X_FORWARDED_FOR
        new_request->indirect_client_addr = old_request->indirect_client_addr;
#endif /* FOLLOW_X_FORWARDED_FOR */
        new_request->my_addr = old_request->my_addr;
        new_request->flags = old_request->flags;
        new_request->flags.redirected = 1;

        if (old_request->auth_user_request) {
            new_request->auth_user_request = old_request->auth_user_request;
            AUTHUSERREQUESTLOCK(new_request->auth_user_request, "new request");
        }

        if (old_request->body_pipe != NULL) {
            new_request->body_pipe = old_request->body_pipe;
            old_request->body_pipe = NULL;
            debugs(0,0,HERE << "redirecting body_pipe " << new_request->body_pipe << " from request " << old_request << " to " << new_request);
        }

        new_request->content_length = old_request->content_length;
        new_request->extacl_user = old_request->extacl_user;
        new_request->extacl_passwd = old_request->extacl_passwd;
        new_request->flags.proxy_keepalive = old_request->flags.proxy_keepalive;
        HTTPMSGUNLOCK(old_request);
        http->request = HTTPMSGLOCK(new_request);
    }

    /* FIXME PIPELINE: This is innacurate during pipelining */

    if (http->getConn() != NULL)
        fd_note(http->getConn()->fd, http->uri);

    assert(http->uri);

    http->doCallouts();
}

/** Test cache allow/deny configuration
 *  Sets flags.cachable=1 if caching is not denied.
 */
void
ClientRequestContext::checkNoCache()
{
    if (Config.accessList.noCache) {
        acl_checklist = clientAclChecklistCreate(Config.accessList.noCache, http);
        acl_checklist->nonBlockingCheck(checkNoCacheDoneWrapper, this);
    } else {
        /* unless otherwise specified, we try to cache. */
        checkNoCacheDone(1);
    }
}

static void
checkNoCacheDoneWrapper(int answer, void *data)
{
    ClientRequestContext *calloutContext = (ClientRequestContext *) data;

    if (!calloutContext->httpStateIsValid())
        return;

    calloutContext->checkNoCacheDone(answer);
}

void
ClientRequestContext::checkNoCacheDone(int answer)
{
    acl_checklist = NULL;
    http->request->flags.cachable = answer;
    http->doCallouts();
}

/*
 * Identify requests that do not go through the store and client side stream
 * and forward them to the appropriate location. All other requests, request
 * them.
 */
void
ClientHttpRequest::processRequest()
{
    debugs(85, 4, "clientProcessRequest: " << RequestMethodStr(request->method) << " '" << uri << "'");

#if USE_SSL
    if (request->method == METHOD_CONNECT && sslBumpNeeded()) {
        sslBumpStart();
        return;
    }
#endif

    if (request->method == METHOD_CONNECT && !redirect.status) {
        logType = LOG_TCP_MISS;
        tunnelStart(this, &out.size, &al.http.code);
        return;
    }

    httpStart();
}

void
ClientHttpRequest::httpStart()
{
    PROF_start(httpStart);
    logType = LOG_TAG_NONE;
    debugs(85, 4, "ClientHttpRequest::httpStart: " << log_tags[logType] << " for '" << uri << "'");

    /* no one should have touched this */
    assert(out.offset == 0);
    /* Use the Stream Luke */
    clientStreamNode *node = (clientStreamNode *)client_stream.tail->data;
    clientStreamRead(node, this, node->readBuffer);
    PROF_stop(httpStart);
}

#if USE_SSL

// determines whether we should bump the CONNECT request
bool
ClientHttpRequest::sslBumpNeeded() const
{
    if (!getConn()->port->sslBump || !Config.accessList.ssl_bump)
        return false;

    debugs(85, 5, HERE << "SslBump possible, checking ACL");

    ACLFilledChecklist check(Config.accessList.ssl_bump, request, NULL);
    check.src_addr = request->client_addr;
    check.my_addr = request->my_addr;
    return check.fastCheck() == 1;
}

// called when comm_write has completed
static void
SslBumpEstablish(int, char *, size_t, comm_err_t errflag, int, void *data)
{
    ClientHttpRequest *r = static_cast<ClientHttpRequest*>(data);
    debugs(85, 5, HERE << "responded to CONNECT: " << r << " ? " << errflag);

    assert(r && cbdataReferenceValid(r));
    r->sslBumpEstablish(errflag);
}

void
ClientHttpRequest::sslBumpEstablish(comm_err_t errflag)
{
    // Bail out quickly on COMM_ERR_CLOSING - close handlers will tidy up
    if (errflag == COMM_ERR_CLOSING)
        return;

    if (errflag) {
        getConn()->startClosing("CONNECT response failure in SslBump");
        return;
    }

    getConn()->switchToHttps();
}

void
ClientHttpRequest::sslBumpStart()
{
    debugs(85, 5, HERE << "ClientHttpRequest::sslBumpStart");

    // send an HTTP 200 response to kick client SSL negotiation
    const int fd = getConn()->fd;
    debugs(33, 7, HERE << "Confirming CONNECT tunnel on FD " << fd);

    // TODO: Unify with tunnel.cc and add a Server(?) header
    static const char *const conn_established =
        "HTTP/1.0 200 Connection established\r\n\r\n";
    comm_write(fd, conn_established, strlen(conn_established),
               &SslBumpEstablish, this, NULL);
}

#endif

bool
ClientHttpRequest::gotEnough() const
{
    /** TODO: should be querying the stream. */
    int64_t contentLength =
        memObject()->getReply()->bodySize(request->method);
    assert(contentLength >= 0);

    if (out.offset < contentLength)
        return false;

    return true;
}

void
ClientHttpRequest::storeEntry(StoreEntry *newEntry)
{
    entry_ = newEntry;
}

void
ClientHttpRequest::loggingEntry(StoreEntry *newEntry)
{
    if (loggingEntry_)
        loggingEntry_->unlock();

    loggingEntry_ = newEntry;

    if (loggingEntry_)
        loggingEntry_->lock();
}

/*
 * doCallouts() - This function controls the order of "callout"
 * executions, including non-blocking access control checks, the
 * redirector, and ICAP.  Previously, these callouts were chained
 * together such that "clientAccessCheckDone()" would call
 * "clientRedirectStart()" and so on.
 *
 * The ClientRequestContext (aka calloutContext) class holds certain
 * state data for the callout/callback operations.  Previously
 * ClientHttpRequest would sort of hand off control to ClientRequestContext
 * for a short time.  ClientRequestContext would then delete itself
 * and pass control back to ClientHttpRequest when all callouts
 * were finished.
 *
 * This caused some problems for ICAP because we want to make the
 * ICAP callout after checking ACLs, but before checking the no_cache
 * list.  We can't stuff the ICAP state into the ClientRequestContext
 * class because we still need the ICAP state after ClientRequestContext
 * goes away.
 *
 * Note that ClientRequestContext is created before the first call
 * to doCallouts().
 *
 * If one of the callouts notices that ClientHttpRequest is no
 * longer valid, it should call cbdataReferenceDone() so that
 * ClientHttpRequest's reference count goes to zero and it will get
 * deleted.  ClientHttpRequest will then delete ClientRequestContext.
 *
 * Note that we set the _done flags here before actually starting
 * the callout.  This is strictly for convenience.
 */

extern int aclMapTOS (acl_tos * head, ACLChecklist * ch);

void
ClientHttpRequest::doCallouts()
{
    assert(calloutContext);

    /*Save the original request for logging purposes*/
    if (!calloutContext->http->al.request)
        calloutContext->http->al.request = HTTPMSGLOCK(request);

    if (!calloutContext->http_access_done) {
        debugs(83, 3, HERE << "Doing calloutContext->clientAccessCheck()");
        calloutContext->http_access_done = true;
        calloutContext->clientAccessCheck();
        return;
    }

#if USE_ADAPTATION
    if (!calloutContext->adaptation_acl_check_done) {
        calloutContext->adaptation_acl_check_done = true;
        if (Adaptation::AccessCheck::Start(
                    Adaptation::methodReqmod, Adaptation::pointPreCache,
                    request, NULL, adaptationAclCheckDoneWrapper, calloutContext))
            return; // will call callback
    }
#endif

    if (!calloutContext->redirect_done) {
        calloutContext->redirect_done = true;
        assert(calloutContext->redirect_state == REDIRECT_NONE);

        if (Config.Program.redirect) {
            debugs(83, 3, HERE << "Doing calloutContext->clientRedirectStart()");
            calloutContext->redirect_state = REDIRECT_PENDING;
            calloutContext->clientRedirectStart();
            return;
        }
    }

    if (!calloutContext->interpreted_req_hdrs) {
        debugs(83, 3, HERE << "Doing clientInterpretRequestHeaders()");
        calloutContext->interpreted_req_hdrs = 1;
        clientInterpretRequestHeaders(this);
    }

    if (!calloutContext->no_cache_done) {
        calloutContext->no_cache_done = true;

        if (Config.accessList.noCache && request->flags.cachable) {
            debugs(83, 3, HERE << "Doing calloutContext->checkNoCache()");
            calloutContext->checkNoCache();
            return;
        }
    }

    if (!calloutContext->clientside_tos_done) {
        calloutContext->clientside_tos_done = true;
        if (getConn() != NULL) {
            ACLFilledChecklist ch(NULL, request, NULL);
            ch.src_addr = request->client_addr;
            ch.my_addr = request->my_addr;
            int tos = aclMapTOS(Config.accessList.clientside_tos, &ch);
            if (tos)
                comm_set_tos(getConn()->fd, tos);
        }
    }

    cbdataReferenceDone(calloutContext->http);
    delete calloutContext;
    calloutContext = NULL;
#if HEADERS_LOG

    headersLog(0, 1, request->method, request);
#endif

    debugs(83, 3, HERE << "calling processRequest()");
    processRequest();

#if ICAP_CLIENT
    Adaptation::Icap::History::Pointer ih = request->icapHistory();
    if (ih != NULL)
        ih->logType = logType;
#endif
}

#ifndef _USE_INLINE_
#include "client_side_request.cci"
#endif

#if USE_ADAPTATION
/// Initiate an asynchronous adaptation transaction which will call us back.
void
ClientHttpRequest::startAdaptation(const Adaptation::ServiceGroupPointer &g)
{
    debugs(85, 3, HERE << "adaptation needed for " << this);
    assert(!virginHeadSource);
    assert(!adaptedBodySource);
    virginHeadSource = initiateAdaptation(
                           new Adaptation::Iterator(this, request, NULL, g));

    // we could try to guess whether we can bypass this adaptation
    // initiation failure, but it should not really happen
    assert(virginHeadSource != NULL); // Must, really
}

void
ClientHttpRequest::noteAdaptationAnswer(HttpMsg *msg)
{
    assert(cbdataReferenceValid(this));		// indicates bug
    assert(msg);

    if (HttpRequest *new_req = dynamic_cast<HttpRequest*>(msg)) {
        /*
         * Replace the old request with the new request.
         */
        HTTPMSGUNLOCK(request);
        request = HTTPMSGLOCK(new_req);
        /*
         * Store the new URI for logging
         */
        xfree(uri);
        uri = xstrdup(urlCanonical(request));
        setLogUri(this, urlCanonicalClean(request));
        assert(request->method.id());
    } else if (HttpReply *new_rep = dynamic_cast<HttpReply*>(msg)) {
        debugs(85,3,HERE << "REQMOD reply is HTTP reply");

        // subscribe to receive reply body
        if (new_rep->body_pipe != NULL) {
            adaptedBodySource = new_rep->body_pipe;
            int consumer_ok = adaptedBodySource->setConsumerIfNotLate(this);
            assert(consumer_ok);
        }

        clientStreamNode *node = (clientStreamNode *)client_stream.tail->prev->data;
        clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
        repContext->createStoreEntry(request->method, request->flags);

        EBIT_CLR(storeEntry()->flags, ENTRY_FWD_HDR_WAIT);
        request_satisfaction_mode = true;
        request_satisfaction_offset = 0;
        storeEntry()->replaceHttpReply(new_rep);
        storeEntry()->timestampsSet();

        if (!adaptedBodySource) // no body
            storeEntry()->complete();
        clientGetMoreData(node, this);
    }

    // we are done with getting headers (but may be receiving body)
    clearAdaptation(virginHeadSource);

    if (!request_satisfaction_mode)
        doCallouts();
}

void
ClientHttpRequest::noteAdaptationQueryAbort(bool final)
{
    clearAdaptation(virginHeadSource);
    assert(!adaptedBodySource);
    handleAdaptationFailure(!final);
}

void
ClientHttpRequest::noteMoreBodyDataAvailable(BodyPipe::Pointer)
{
    assert(request_satisfaction_mode);
    assert(adaptedBodySource != NULL);

    if (const size_t contentSize = adaptedBodySource->buf().contentSize()) {
        BodyPipeCheckout bpc(*adaptedBodySource);
        const StoreIOBuffer ioBuf(&bpc.buf, request_satisfaction_offset);
        storeEntry()->write(ioBuf);
        // assume can write everything
        request_satisfaction_offset += contentSize;
        bpc.buf.consume(contentSize);
        bpc.checkIn();
    }

    if (adaptedBodySource->exhausted())
        endRequestSatisfaction();
    // else wait for more body data
}

void
ClientHttpRequest::noteBodyProductionEnded(BodyPipe::Pointer)
{
    assert(!virginHeadSource);
    if (adaptedBodySource != NULL) { // did not end request satisfaction yet
        // We do not expect more because noteMoreBodyDataAvailable always
        // consumes everything. We do not even have a mechanism to consume
        // leftovers after noteMoreBodyDataAvailable notifications seize.
        assert(adaptedBodySource->exhausted());
        endRequestSatisfaction();
    }
}

void
ClientHttpRequest::endRequestSatisfaction()
{
    debugs(85,4, HERE << this << " ends request satisfaction");
    assert(request_satisfaction_mode);
    stopConsumingFrom(adaptedBodySource);

    // TODO: anything else needed to end store entry formation correctly?
    storeEntry()->complete();
}

void
ClientHttpRequest::noteBodyProducerAborted(BodyPipe::Pointer)
{
    assert(!virginHeadSource);
    stopConsumingFrom(adaptedBodySource);
    handleAdaptationFailure();
}

void
ClientHttpRequest::handleAdaptationFailure(bool bypassable)
{
    debugs(85,3, HERE << "handleAdaptationFailure(" << bypassable << ")");

    const bool usedStore = storeEntry() && !storeEntry()->isEmpty();
    const bool usedPipe = request->body_pipe != NULL &&
                          request->body_pipe->consumedSize() > 0;

    if (bypassable && !usedStore && !usedPipe) {
        debugs(85,3, HERE << "ICAP REQMOD callout failed, bypassing: " << calloutContext);
        if (calloutContext)
            doCallouts();
        return;
    }

    debugs(85,3, HERE << "ICAP REQMOD callout failed, responding with error");

    clientStreamNode *node = (clientStreamNode *)client_stream.tail->prev->data;
    clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
    assert(repContext);

    // The original author of the code also wanted to pass an errno to
    // setReplyToError, but it seems unlikely that the errno reflects the
    // true cause of the error at this point, so I did not pass it.
    IpAddress noAddr;
    noAddr.SetNoAddr();
    ConnStateData * c = getConn();
    repContext->setReplyToError(ERR_ICAP_FAILURE, HTTP_INTERNAL_SERVER_ERROR,
                                request->method, NULL,
                                (c != NULL ? c->peer : noAddr), request, NULL,
                                (c != NULL && c->auth_user_request ?
                                 c->auth_user_request : request->auth_user_request));

    node = (clientStreamNode *)client_stream.tail->data;
    clientStreamRead(node, this, node->readBuffer);
}

#endif
