/* WebDAV internals for CalDAV. */
#ifndef DAV_H
#define DAV_H

#include <libxml/xpath.h>

#include "calsync.h"

#define NS_DAV    "DAV:"
#define NS_CALDAV "urn:ietf:params:xml:ns:caldav"

typedef struct {
	Account *a;
	SyncRun *run;
	HttpRes res;
	xmlDocPtr doc;
	xmlXPathContextPtr xp;
} Dav;

HttpRes dav_send(const Account *a, HttpReq q);
int dav_query(Dav *d, HttpReq q);
void dav_reset(Dav *d);
xmlChar *dav_text(const Dav *d, xmlNodePtr at, const char *expr);
int dav_count(const Dav *d, xmlNodePtr at, const char *expr);
char *dav_href(const Dav *d, xmlNodePtr at, const char *expr);
int dav_list_home(Dav *d);
int dav_fail(SyncRun *run, const HttpRes *res);
int dav_refuse(SyncRun *run, const char *why);

#endif
