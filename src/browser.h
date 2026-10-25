#ifndef BROWSER_H
#define BROWSER_H

#define BROWSER_MAX_LINES 500
#define BROWSER_LINE_LEN  96

int browser_navigate(const char *url);

int         browser_line_count(void);
const char *browser_line_at(int i);
const char *browser_title(void);
const char *browser_status(void);
const char *browser_url(void);

#endif
