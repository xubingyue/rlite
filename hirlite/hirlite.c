#include "constants.h"
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hirlite.h"
#include "util.h"

#define UNSIGN(val) ((unsigned char *)val)

#define RLITE_SERVER_ERR(c, retval)\
	if (retval == RL_WRONG_TYPE) {\
		c->reply = createErrorObject(RLITE_WRONGTYPEERR);\
		goto cleanup;\
	}\
	if (retval == RL_NAN) {\
		c->reply = createErrorObject("resulting score is not a number (NaN)");\
		goto cleanup;\
	}\

static struct rliteCommand *lookupCommand(const char *name, size_t UNUSED(len));
static void __rliteSetError(rliteContext *c, int type, const char *str);

static int catvprintf(char** s, size_t *slen, const char *fmt, va_list ap) {
	va_list cpy;
	char *buf, *t;
	size_t buflen = 16;

	while(1) {
		buf = malloc(buflen);
		if (buf == NULL) return RLITE_ERR;
		buf[buflen-2] = '\0';
		va_copy(cpy,ap);
		vsnprintf(buf, buflen, fmt, cpy);
		if (buf[buflen-2] != '\0') {
			free(buf);
			buflen *= 2;
			continue;
		}
		break;
	}
	t = realloc(*s, sizeof(char) * (*slen + buflen));
	if (!t) {
		free(buf);
		return RLITE_ERR;
	}
	memmove(&t[*slen], buf, buflen);
	free(buf);
	return RLITE_OK;
}

static rliteReply *createReplyObject(int type) {
	rliteReply *r = calloc(1,sizeof(*r));

	if (r == NULL)
		return NULL;

	r->type = type;
	return r;
}

static rliteReply *createStringTypeObject(int type, const char *str, const int len) {
	rliteReply *reply = createReplyObject(type);
	reply->str = malloc(sizeof(char) * (len + 1));
	if (!reply->str) {
		rliteFreeReplyObject(reply);
		return NULL;
	}
	memcpy(reply->str, str, len);
	reply->str[len] = 0;
	reply->len = len;
	return reply;
}

static rliteReply *createStringObject(const char *str, const int len) {
	return createStringTypeObject(RLITE_REPLY_STRING, str, len);
}

static rliteReply *createCStringObject(const char *str) {
	return createStringObject(str, strlen(str));
}

static rliteReply *createErrorObject(const char *str) {
	return createStringTypeObject(RLITE_REPLY_ERROR, str, strlen((char *)str));
}

static rliteReply *createStatusObject(const char *str) {
	return createStringTypeObject(RLITE_REPLY_STATUS, str, strlen((char *)str));
}

static rliteReply *createDoubleObject(double d) {
	char dbuf[128];
	int dlen;
	if (isinf(d)) {
		/* Libc in odd systems (Hi Solaris!) will format infinite in a
		 * different way, so better to handle it in an explicit way. */
		return createCStringObject(d > 0 ? "inf" : "-inf");
	} else {
		dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
		return createStringObject(dbuf, dlen);
	}
}

static rliteReply *createLongLongObject(long long value) {
	rliteReply *reply = createReplyObject(RLITE_REPLY_INTEGER);
	reply->integer = value;
	return reply;
}

static void addZsetIteratorReply(rliteClient *c, int retval, rl_zset_iterator *iterator, int withscores)
{
	unsigned char *vstr;
	long vlen, i;
	double score;

	c->reply = createReplyObject(RLITE_REPLY_ARRAY);
	if (retval == RL_NOT_FOUND) {
		c->reply->elements = 0;
		return;
	}
	c->reply->elements = withscores ? (iterator->size * 2) : iterator->size;
	c->reply->element = malloc(sizeof(rliteReply*) * c->reply->elements);
	i = 0;
	while ((retval = rl_zset_iterator_next(iterator, withscores ? &score : NULL, &vstr, &vlen)) == RL_OK) {
		c->reply->element[i] = createStringObject((char *)vstr, vlen);
		i++;
		if (withscores) {
			c->reply->element[i] = createDoubleObject(score);
			i++;
		}
		rl_free(vstr);
	}

	if (retval != RL_END) {
		__rliteSetError(c->context, RLITE_ERR, "Unexpected early end");
		goto cleanup;
	}
	iterator = NULL;
cleanup:
	if (iterator) {
		rl_zset_iterator_destroy(iterator);
	}
}

static int addReply(rliteContext* c, rliteReply *reply) {
	if (c->replyPosition == c->replyAlloc) {
		void *tmp;
		c->replyAlloc *= 2;
		tmp = realloc(c->replies, sizeof(rliteReply*) * c->replyAlloc);
		if (!tmp) {
			__rliteSetError(c,RLITE_ERR_OOM,"Out of memory");
			return RLITE_ERR;
		}
		c->replies = tmp;
	}

	c->replies[c->replyPosition] = reply;
	c->replyLength++;
	return RLITE_OK;
}

static int addReplyStatusFormat(rliteContext *c, const char *fmt, ...) {
	int maxlen = strlen(fmt) * 2;
	char *str = malloc(maxlen * sizeof(char));
	va_list ap;
	va_start(ap, fmt);
	int written = vsnprintf(str, maxlen, fmt, ap);
	va_end(ap);
	if (written < 0) {
		fprintf(stderr, "Failed to vsnprintf near line %d, got %d\n", __LINE__, written);
		free(str);
		return RLITE_ERR;
	}
	rliteReply *reply = createReplyObject(RLITE_REPLY_STATUS);
	if (!reply) {
		free(str);
		__rliteSetError(c,RLITE_ERR_OOM,"Out of memory");
		return RLITE_ERR;
	}
	reply->str = str;
	addReply(c, reply);
	return RLITE_OK;
}

static int addReplyErrorFormat(rliteContext *c, const char *fmt, ...) {
	int maxlen = strlen(fmt) * 2;
	char *str = malloc(maxlen * sizeof(char));
	va_list ap;
	va_start(ap, fmt);
	int written = vsnprintf(str, maxlen, fmt, ap);
	va_end(ap);
	if (written < 0) {
		fprintf(stderr, "Failed to vsnprintf near line %d, got %d\n", __LINE__, written);
		free(str);
		return RLITE_ERR;
	}
	rliteReply *reply = createReplyObject(RLITE_REPLY_ERROR);
	if (!reply) {
		free(str);
		__rliteSetError(c,RLITE_ERR_OOM,"Out of memory");
		return RLITE_ERR;
	}
	reply->str = str;
	addReply(c, reply);
	return RLITE_OK;
}

static int getDoubleFromObject(const char *o, double *target) {
	double value;
	char *eptr;

	if (o == NULL) {
		value = 0;
	} else {
		errno = 0;
		value = strtod(o, &eptr);
		if (isspace(((char*)o)[0]) || eptr[0] != '\0' ||
				(errno == ERANGE && (value == HUGE_VAL ||
									 value == -HUGE_VAL || value == 0)) ||
				errno == EINVAL || isnan(value))
		return RLITE_ERR;
	}
	*target = value;
	return RLITE_OK;
}

static int getDoubleFromObjectOrReply(rliteClient *c, const char *o, double *target, const char *msg) {
	if (getDoubleFromObject(o, target) != RLITE_OK) {
		if (msg != NULL) {
			c->reply = createErrorObject(msg);
		} else {
			c->reply = createErrorObject("value is not a valid float");
		}
		return RLITE_ERR;
	}
	return RLITE_OK;
}

static int getLongLongFromObject(const char *o, long long *target) {
	long long value;
	char *eptr;

	if (o == NULL) {
		value = 0;
	} else {
		errno = 0;
		value = strtoll(o, &eptr, 10);
		if (isspace(((char*)o)[0]) || eptr[0] != '\0' || errno == ERANGE) {
			return RLITE_ERR;
		}
	}
	if (target) *target = value;
	return RLITE_OK;
}

static int getLongLongFromObjectOrReply(rliteClient *c, const char *o, long long *target, const char *msg) {
	long long value;
	if (getLongLongFromObject(o, &value) != RLITE_OK) {
		if (msg != NULL) {
			c->reply = createErrorObject(msg);
		} else {
			c->reply = createErrorObject("value is not an integer or out of range");
		}
		return RLITE_ERR;
	}
	*target = value;
	return RLITE_OK;
}

static int getLongFromObjectOrReply(rliteClient *c, const char *o, long *target, const char *msg) {
	long long value;

	if (getLongLongFromObjectOrReply(c, o, &value, msg) != RLITE_OK) return RLITE_ERR;
	if (value < LONG_MIN || value > LONG_MAX) {
		if (msg != NULL) {
			c->reply = createErrorObject(msg);
		} else {
			c->reply = createErrorObject("value is out of range");
		}
		return RLITE_ERR;
	}
	*target = value;
	return RLITE_OK;
}
static void __rliteSetError(rliteContext *c, int type, const char *str) {
	size_t len;

	c->err = type;
	if (str != NULL) {
		len = strlen(str);
		len = len < (sizeof(c->errstr)-1) ? len : (sizeof(c->errstr)-1);
		memcpy(c->errstr,str,len);
		c->errstr[len] = '\0';
	} else {
		/* Only RLITE_ERR_IO may lack a description! */
		assert(type == RLITE_ERR_IO);
		strerror_r(errno,c->errstr,sizeof(c->errstr));
	}
}
/* Free a reply object */
void rliteFreeReplyObject(void *reply) {
	rliteReply *r = reply;
	size_t j;

	if (r == NULL)
		return;

	switch(r->type) {
	case RLITE_REPLY_INTEGER:
		break; /* Nothing to free */
	case RLITE_REPLY_ARRAY:
		if (r->element != NULL) {
			for (j = 0; j < r->elements; j++)
				if (r->element[j] != NULL)
					rliteFreeReplyObject(r->element[j]);
			free(r->element);
		}
		break;
	case RLITE_REPLY_ERROR:
	case RLITE_REPLY_STATUS:
	case RLITE_REPLY_STRING:
		if (r->str != NULL)
			free(r->str);
		break;
	}
	free(r);
}

int rlitevFormatCommand(rliteClient *client, const char *format, va_list ap) {
	const char *c = format;
	char *curarg, *newarg; /* current argument */
	size_t curarglen;
	int touched = 0; /* was the current argument touched? */
	char **curargv = NULL, **newargv = NULL;
	size_t *curargvlen = NULL, *newargvlen = NULL;
	int argc = 0;

	/* Build the command string accordingly to protocol */
	curarg = NULL;
	curarglen = 0;

	while(*c != '\0') {
		if (*c != '%' || c[1] == '\0') {
			if (*c == ' ') {
				if (touched) {
					newargv = realloc(curargv, sizeof(char*)*(argc+1));
					if (newargv == NULL) goto err;
					newargvlen = realloc(curargvlen, sizeof(size_t)*(argc+1));
					if (newargvlen == NULL) goto err;
					curargv = newargv;
					curargvlen = newargvlen;
					curargv[argc] = curarg;
					curargvlen[argc++] = curarglen;

					/* curarg is put in argv so it can be overwritten. */
					curarg = NULL;
					curarglen = 0;
					touched = 0;
				}
			} else {
				newarg = realloc(curarg, sizeof(char) * (curarglen + 1));
				if (newarg == NULL) goto err;
				newarg[curarglen++] = *c;
				curarg = newarg;
				touched = 1;
			}
		} else {
			char *arg;
			size_t size;

			/* Set newarg so it can be checked even if it is not touched. */
			newarg = curarg;

			switch(c[1]) {
			case 's':
				arg = va_arg(ap,char*);
				size = strlen(arg);
				if (size > 0) {
					newarg = realloc(curarg, sizeof(char) * (curarglen + size));
					memcpy(&newarg[curarglen], arg, size);
					curarglen += size;
				}
				break;
			case 'b':
				arg = va_arg(ap,char*);
				size = va_arg(ap,size_t);
				if (size > 0) {
					newarg = realloc(curarg, sizeof(char) * (curarglen + size));
					memcpy(&newarg[curarglen], arg, size);
					curarglen += size;
				}
				break;
			case '%':
				newarg = realloc(curarg, sizeof(char) * (curarglen + 1));
				newarg[curarglen] = '%';
				curarglen += 1;
				break;
			default:
				/* Try to detect printf format */
				{
					static const char intfmts[] = "diouxX";
					char _format[16];
					const char *_p = c+1;
					size_t _l = 0;
					va_list _cpy;

					/* Flags */
					if (*_p != '\0' && *_p == '#') _p++;
					if (*_p != '\0' && *_p == '0') _p++;
					if (*_p != '\0' && *_p == '-') _p++;
					if (*_p != '\0' && *_p == ' ') _p++;
					if (*_p != '\0' && *_p == '+') _p++;

					/* Field width */
					while (*_p != '\0' && isdigit(*_p)) _p++;

					/* Precision */
					if (*_p == '.') {
						_p++;
						while (*_p != '\0' && isdigit(*_p)) _p++;
					}

					/* Copy va_list before consuming with va_arg */
					va_copy(_cpy,ap);

					/* Integer conversion (without modifiers) */
					if (strchr(intfmts,*_p) != NULL) {
						va_arg(ap,int);
						goto fmt_valid;
					}

					/* Double conversion (without modifiers) */
					if (strchr("eEfFgGaA",*_p) != NULL) {
						va_arg(ap,double);
						goto fmt_valid;
					}

					/* Size: char */
					if (_p[0] == 'h' && _p[1] == 'h') {
						_p += 2;
						if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
							va_arg(ap,int); /* char gets promoted to int */
							goto fmt_valid;
						}
						goto fmt_invalid;
					}

					/* Size: short */
					if (_p[0] == 'h') {
						_p += 1;
						if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
							va_arg(ap,int); /* short gets promoted to int */
							goto fmt_valid;
						}
						goto fmt_invalid;
					}

					/* Size: long long */
					if (_p[0] == 'l' && _p[1] == 'l') {
						_p += 2;
						if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
							va_arg(ap,long long);
							goto fmt_valid;
						}
						goto fmt_invalid;
					}

					/* Size: long */
					if (_p[0] == 'l') {
						_p += 1;
						if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
							va_arg(ap,long);
							goto fmt_valid;
						}
						goto fmt_invalid;
					}

				fmt_invalid:
					va_end(_cpy);
					goto err;

				fmt_valid:
					_l = (_p+1)-c;
					if (_l < sizeof(_format)-2) {
						memcpy(_format,c,_l);
						_format[_l] = '\0';
						newarg = curarg;
						if (RLITE_ERR == catvprintf(&newarg, &curarglen, _format,_cpy)) {
							goto err;
						}

						c = _p-1;
					}

					va_end(_cpy);
					break;
				}
			}

			if (newarg == NULL) goto err;
			curarg = newarg;

			touched = 1;
			c++;
		}
		c++;
	}

	/* Add the last argument if needed */
	if (touched) {
		newargv = realloc(curargv,sizeof(char*)*(argc+1));
		if (newargv == NULL) goto err;
		newargvlen = realloc(curargvlen,sizeof(char*)*(argc+1));
		if (newargvlen == NULL) goto err;
		curargv = newargv;
		curargvlen = newargvlen;
		curargv[argc] = curarg;
		curargvlen[argc++] = curarglen;
	} else {
		free(curarg);
	}

	/* Clear curarg because it was put in curargv or was free'd. */
	curarg = NULL;

	client->argc = argc;
	client->argv = curargv;
	client->argvlen = curargvlen;
	return RLITE_OK;
err:
	while(argc--)
		free(curargv[argc]);
	free(curargv);

	if (curarg != NULL)
		free(curarg);

	return RLITE_ERR;
}

int rliteFormatCommand(rliteClient *client, const char *format, ...) {
	va_list ap;
	int retval;
	va_start(ap,format);
	retval = rlitevFormatCommand(client, format, ap);
	va_end(ap);
	return retval;
}

int rliteFormatCommandArgv(rliteClient *client, int argc, char **argv, size_t *argvlen) {
	client->argc = argc;
	client->argv = argv;
	client->argvlen = argvlen;
	return RLITE_OK;
}

#define DEFAULT_REPLIES_SIZE 16
static rliteContext *_rliteConnect(const char *path) {
	rliteContext *context = malloc(sizeof(*context));
	if (!context) {
		return NULL;
	}
	context->replies = malloc(sizeof(rliteReply*) * DEFAULT_REPLIES_SIZE);
	if (!context->replies) {
		free(context);
		context = NULL;
		goto cleanup;
	}
	context->replyPosition = 0;
	context->replyLength = 0;
	context->replyAlloc = DEFAULT_REPLIES_SIZE;
	context->debugSkiplist = 0;
	context->hashtableLimitEntries = 0;
	context->hashtableLimitValue = 0;
	int retval = rl_open(path, &context->db, RLITE_OPEN_READWRITE | RLITE_OPEN_CREATE);
	if (retval != RL_OK) {
		free(context);
		context = NULL;
		goto cleanup;
	}
cleanup:
	return context;
}

rliteContext *rliteConnect(const char *ip, int UNUSED(port)) {
	return _rliteConnect(ip);
}

rliteContext *rliteConnectWithTimeout(const char *ip, int UNUSED(port), const struct timeval UNUSED(tv)) {
	return _rliteConnect(ip);
}

rliteContext *rliteConnectNonBlock(const char *ip, int UNUSED(port)) {
	return _rliteConnect(ip);
}

rliteContext *rliteConnectBindNonBlock(const char *ip, int UNUSED(port), const char *UNUSED(source_addr)) {
	return _rliteConnect(ip);
}

rliteContext *rliteConnectUnix(const char *path) {
	return _rliteConnect(path);
}

rliteContext *rliteConnectUnixWithTimeout(const char *path, const struct timeval UNUSED(tv)) {
	return _rliteConnect(path);
}

rliteContext *rliteConnectUnixNonBlock(const char *path) {
	return _rliteConnect(path);
}

rliteContext *rliteConnectFd(int UNUSED(fd)) {
	return NULL;
}
int rliteSetTimeout(rliteContext *UNUSED(c), const struct timeval UNUSED(tv)) {
	return 0;
}

int rliteEnableKeepAlive(rliteContext *UNUSED(c)) {
	return 0;
}
void rliteFree(rliteContext *c) {
	rl_close(c->db);
	int i;
	for (i = c->replyPosition; i < c->replyLength; i++) {
		rliteFreeReplyObject(c->replies[i]);
	}
	free(c->replies);
	free(c);
}

int rliteFreeKeepFd(rliteContext *UNUSED(c)) {
	return 0;
}

int rliteBufferRead(rliteContext *UNUSED(c)) {
	return 0;
}

int rliteBufferWrite(rliteContext *UNUSED(c), int *UNUSED(done)) {
	return 0;
}

static void *_popReply(rliteContext *c) {
	if (c->replyPosition < c->replyLength) {
		void *ret;
		ret = c->replies[c->replyPosition];
		c->replyPosition++;
		if (c->replyPosition == c->replyLength) {
			c->replyPosition = c->replyLength = 0;
		}
		return ret;
	} else {
		return NULL;
	}
}

int rliteGetReply(rliteContext *c, void **reply) {
	*reply = _popReply(c);
	return RLITE_OK;
}

static int __rliteAppendCommandClient(rliteClient *client) {
	if (client->argc == 0) {
		return RLITE_ERR;
	}

	struct rliteCommand *command = lookupCommand(client->argv[0], client->argvlen[0]);
	int retval = RLITE_OK;
	if (!command) {
		retval = addReplyErrorFormat(client->context, "unknown command '%s'", (char*)client->argv[0]);
	} else if ((command->arity > 0 && command->arity != client->argc) ||
		(client->argc < -command->arity)) {
		retval = addReplyErrorFormat(client->context, "wrong number of arguments for '%s' command", command->name);
	} else {
		client->reply = NULL;
		command->proc(client);
		if (client->reply) {
			retval = addReply(client->context, client->reply);
		}
	}
	return retval;
}

int rliteAppendFormattedCommand(rliteContext *UNUSED(c), const char *UNUSED(cmd), size_t UNUSED(len)) {
	// Not implemented
	// cmd is formatted for redis server, and we don't have a parser for it... yet.
	return RLITE_ERR;
}

int rlitevAppendCommand(rliteContext *c, const char *format, va_list ap) {
	rliteClient client;
	client.context = c;
	if (rlitevFormatCommand(&client, format, ap) != RLITE_OK) {
		return RLITE_ERR;
	}

	int retval = __rliteAppendCommandClient(&client);
	int i;
	for (i = 0; i < client.argc; i++) {
		free(client.argv[i]);
	}
	free(client.argv);
	free(client.argvlen);

	return retval;
}

int rliteAppendCommand(rliteContext *c, const char *format, ...) {
	va_list ap;
	int retval;
	va_start(ap,format);
	retval = rlitevAppendCommand(c, format, ap);
	va_end(ap);
	return retval;
}

int rliteAppendCommandArgv(rliteContext *c, int argc, char **argv, size_t *argvlen) {
	rliteClient client;
	client.context = c;
	client.argc = argc;
	client.argv = argv;
	client.argvlen = argvlen;

	return __rliteAppendCommandClient(&client);
}

void *rlitevCommand(rliteContext *c, const char *format, va_list ap) {
	if (rlitevAppendCommand(c,format,ap) != RLITE_OK)
		return NULL;
	return _popReply(c);
}

void *rliteCommand(rliteContext *c, const char *format, ...) {
	va_list ap;
	void *reply = NULL;
	va_start(ap,format);
	reply = rlitevCommand(c,format,ap);
	va_end(ap);
	return reply;
}

void *rliteCommandArgv(rliteContext *c, int argc, char **argv, size_t *argvlen) {
	if (rliteAppendCommandArgv(c,argc,argv,argvlen) != RLITE_OK)
		return NULL;
	return _popReply(c);
}

static void echoCommand(rliteClient *c)
{
	c->reply = createStringObject(c->argv[1], c->argvlen[1]);
}

static void pingCommand(rliteClient *c)
{
	c->reply = createStringObject("PONG", 4);
}

static void zaddGenericCommand(rliteClient *c, int incr) {
	const unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	double score = 0, *scores = NULL;
	int j, elements = (c->argc - 2) / 2;
	int added = 0;

	if (c->argc % 2) {
		c->reply = createErrorObject(RLITE_SYNTAXERR);
		return;
	}

	/* Start parsing all the scores, we need to emit any syntax error
	 * before executing additions to the sorted set, as the command should
	 * either execute fully or nothing at all. */
	scores = malloc(sizeof(double) * elements);
	for (j = 0; j < elements; j++) {
		if (getDoubleFromObjectOrReply(c,c->argv[2+j*2],&scores[j],NULL)
			!= RLITE_OK) goto cleanup;
	}

	int retval;
	for (j = 0; j < elements; j++) {
		score = scores[j];
		if (incr) {
			retval = rl_zincrby(c->context->db, key, keylen, score, UNSIGN(c->argv[3+j*2]), c->argvlen[3+j*2], NULL);
			RLITE_SERVER_ERR(c, retval);
		} else {
			retval = rl_zadd(c->context->db, key, keylen, score, UNSIGN(c->argv[3+j*2]), c->argvlen[3+j*2]);
			RLITE_SERVER_ERR(c, retval);
			if (retval == RL_OK) {
				added++;
			}
		}
	}
	if (incr) /* ZINCRBY */
		c->reply = createDoubleObject(score);
	else /* ZADD */
		c->reply = createLongLongObject(added);

cleanup:
	free(scores);
}

static void zaddCommand(rliteClient *c) {
	zaddGenericCommand(c,0);
}

static void zincrbyCommand(rliteClient *c) {
	zaddGenericCommand(c,1);
}

static void zrangeGenericCommand(rliteClient *c, int reverse) {
	rl_zset_iterator *iterator;
	int withscores = 0;
	long start;
	long end;

	if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != RLITE_OK) ||
		(getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != RLITE_OK)) return;

	if (c->argc == 5 && !strcasecmp(c->argv[4], "withscores")) {
		withscores = 1;
	} else if (c->argc >= 5) {
		c->reply = createErrorObject(RLITE_SYNTAXERR);
		return;
	}

	int retval = (reverse ? rl_zrevrange : rl_zrange)(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], start, end, &iterator);
	addZsetIteratorReply(c, retval, iterator, withscores);
}

static void zrangeCommand(rliteClient *c) {
	zrangeGenericCommand(c, 0);
}

static void zrevrangeCommand(rliteClient *c) {
	zrangeGenericCommand(c, 1);
}

static void zremCommand(rliteClient *c) {
	const unsigned char *key = UNSIGN(c->argv[1]);
	const size_t keylen = c->argvlen[1];
	long deleted = 0;
	int j;

	// memberslen needs long, we have size_t (unsigned long)
	// it would be great not to need this
	long *memberslen = malloc(sizeof(long) * (c->argc - 2));
	if (!memberslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (j = 2; j < c->argc; j++) {
		memberslen[j - 2] = c->argvlen[j];
	}
	int retval = rl_zrem(c->context->db, key, keylen, c->argc - 2, (unsigned char **)&c->argv[2], memberslen, &deleted);
	free(memberslen);
	RLITE_SERVER_ERR(c, retval);

	c->reply = createLongLongObject(deleted);
cleanup:
	return;
}

/* Populate the rangespec according to the objects min and max. */
static int zslParseRange(const char *min, const char *max, rl_zrangespec *spec) {
	char *eptr;
	spec->minex = spec->maxex = 0;

	/* Parse the min-max interval. If one of the values is prefixed
	 * by the "(" character, it's considered "open". For instance
	 * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
	 * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
	if (min[0] == '(') {
		spec->min = strtod((char*)min+1,&eptr);
		if (eptr[0] != '\0' || isnan(spec->min)) return RLITE_ERR;
		spec->minex = 1;
	} else {
		spec->min = strtod((char*)min,&eptr);
		if (eptr[0] != '\0' || isnan(spec->min)) return RLITE_ERR;
	}
	if (((char*)max)[0] == '(') {
		spec->max = strtod((char*)max+1,&eptr);
		if (eptr[0] != '\0' || isnan(spec->max)) return RLITE_ERR;
		spec->maxex = 1;
	} else {
		spec->max = strtod((char*)max,&eptr);
		if (eptr[0] != '\0' || isnan(spec->max)) return RLITE_ERR;
	}

	return RLITE_OK;
}
/* Implements ZREMRANGEBYRANK, ZREMRANGEBYSCORE, ZREMRANGEBYLEX commands. */
#define ZRANGE_RANK 0
#define ZRANGE_SCORE 1
#define ZRANGE_LEX 2
static void zremrangeGenericCommand(rliteClient *c, int rangetype) {
	int retval;
	long deleted;
	rl_zrangespec rlrange;
	long start, end;

	/* Step 1: Parse the range. */
	if (rangetype == ZRANGE_RANK) {
		if ((getLongFromObjectOrReply(c,c->argv[2],&start,NULL) != RLITE_OK) ||
			(getLongFromObjectOrReply(c,c->argv[3],&end,NULL) != RLITE_OK))
			return;
		retval = rl_zremrangebyrank(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], start, end, &deleted);
	} else if (rangetype == ZRANGE_SCORE) {
		if (zslParseRange(c->argv[2],c->argv[3],&rlrange) != RLITE_OK) {
			c->reply = createErrorObject("min or max is not a float");
			return;
		}
		retval = rl_zremrangebyscore(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &rlrange, &deleted);
	} else if (rangetype == ZRANGE_LEX) {
		retval = rl_zremrangebylex(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], UNSIGN(c->argv[3]), c->argvlen[3], &deleted);
	} else {
		__rliteSetError(c->context, RLITE_ERR, "Unexpected rangetype");
		goto cleanup;
	}
	RLITE_SERVER_ERR(c, retval);

	c->reply = createLongLongObject(deleted);
cleanup:
	return;
}

static void zremrangebyrankCommand(rliteClient *c) {
	zremrangeGenericCommand(c,ZRANGE_RANK);
}

static void zremrangebyscoreCommand(rliteClient *c) {
	zremrangeGenericCommand(c,ZRANGE_SCORE);
}

static void zremrangebylexCommand(rliteClient *c) {
	zremrangeGenericCommand(c,ZRANGE_LEX);
}

static void zcardCommand(rliteClient *c) {
	long card = 0;
	int retval = rl_zcard(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &card);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(card);
cleanup:
	return;
}
#define RLITE_AGGR_SUM 1
#define RLITE_AGGR_MIN 2
#define RLITE_AGGR_MAX 3
#define RLITE_OP_UNION 1
#define RLITE_OP_INTER 2

static void zunionInterGenericCommand(rliteClient *c, int op) {
	int i, j;
	long setnum;
	int aggregate = RL_ZSET_AGGREGATE_SUM;
	double *weights = NULL;

	/* expect setnum input keys to be given */
	if ((getLongFromObjectOrReply(c, c->argv[2], &setnum, NULL) != RLITE_OK))
		return;

	if (setnum < 1) {
		c->reply = createErrorObject("at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE");
		return;
	}

	/* test if the expected number of keys would overflow */
	if (setnum > c->argc - 3) {
		c->reply = createErrorObject(RLITE_SYNTAXERR);
		return;
	}

	if (c->argc > 3 + setnum) {
		j = 3 + setnum;
		if (strcasecmp(c->argv[j], "weights") == 0) {
			if (j + 1 + setnum > c->argc) {
				c->reply = createErrorObject(RLITE_SYNTAXERR);
				return;
			}
			weights = malloc(sizeof(double) * setnum);
			for (i = 0; i < setnum; i++) {
				if (getDoubleFromObjectOrReply(c,c->argv[j + 1 + i], &weights[i],
						"weight value is not a float") != RLITE_OK) {
					free(weights);
					return;
				}
			}
			j += setnum + 1;
		}
		if (c->argc > j && c->argvlen[j] == 9 && memcmp(c->argv[j], "aggregate", 9) == 0) {
			if (strcasecmp(c->argv[j + 1], "min") == 0) {
				aggregate = RL_ZSET_AGGREGATE_MIN;
			} else if (strcasecmp(c->argv[j + 1], "max") == 0) {
				aggregate = RL_ZSET_AGGREGATE_MAX;
			} else if (strcasecmp(c->argv[j + 1], "sum") == 0) {
				aggregate = RL_ZSET_AGGREGATE_SUM;
			} else {
				free(weights);
				c->reply = createErrorObject(RLITE_SYNTAXERR);
				return;
			}
			j += 2;
		}
		if (j != c->argc) {
			free(weights);
			c->reply = createErrorObject(RLITE_SYNTAXERR);
			return;
		}
	}

	unsigned char **keys = malloc(sizeof(unsigned char *) * (1 + setnum));
	long *keys_len = malloc(sizeof(long) * (1 + setnum));
	keys[0] = UNSIGN(c->argv[1]);
	keys_len[0] = (long)c->argvlen[1];
	for (i = 0; i < setnum; i++) {
		keys[i + 1] = UNSIGN(c->argv[3 + i]);
		keys_len[i + 1] = c->argvlen[3 + i];
	}
	int retval = (op == RLITE_OP_UNION ? rl_zunionstore : rl_zinterstore)(c->context->db, setnum + 1, keys, keys_len, weights, aggregate);
	free(keys);
	free(keys_len);
	free(weights);
	RLITE_SERVER_ERR(c, retval);
	zcardCommand(c);
cleanup:
	return;
}

static void zunionstoreCommand(rliteClient *c) {
	zunionInterGenericCommand(c, RLITE_OP_UNION);
}

static void zinterstoreCommand(rliteClient *c) {
	zunionInterGenericCommand(c, RLITE_OP_INTER);
}

/* This command implements ZRANGEBYSCORE, ZREVRANGEBYSCORE. */
static void genericZrangebyscoreCommand(rliteClient *c, int reverse) {
	rl_zrangespec range;
	long offset = 0, limit = -1;
	int withscores = 0;
	int minidx, maxidx;

	/* Parse the range arguments. */
	if (reverse) {
		/* Range is given as [max,min] */
		maxidx = 2; minidx = 3;
	} else {
		/* Range is given as [min,max] */
		minidx = 2; maxidx = 3;
	}

	if (zslParseRange(c->argv[minidx],c->argv[maxidx],&range) != RLITE_OK) {
		c->reply = createErrorObject("min or max is not a float");
		return;
	}

	/* Parse optional extra arguments. Note that ZCOUNT will exactly have
	 * 4 arguments, so we'll never enter the following code path. */
	if (c->argc > 4) {
		int remaining = c->argc - 4;
		int pos = 4;

		while (remaining) {
			if (remaining >= 1 && !strcasecmp(c->argv[pos],"withscores")) {
				pos++; remaining--;
				withscores = 1;
			} else if (remaining >= 3 && !strcasecmp(c->argv[pos],"limit")) {
				if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL) != RLITE_OK) ||
					(getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL) != RLITE_OK)) return;
				pos += 3; remaining -= 3;
			} else {
				c->reply = createErrorObject(RLITE_SYNTAXERR);
				return;
			}
		}
	}

	rl_zset_iterator *iterator;
	int retval = (reverse ? rl_zrevrangebyscore : rl_zrangebyscore)(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &range, offset, limit, &iterator);
	addZsetIteratorReply(c, retval, iterator, withscores);
}

static void zrangebyscoreCommand(rliteClient *c) {
	genericZrangebyscoreCommand(c, 0);
}

static void zrevrangebyscoreCommand(rliteClient *c) {
	genericZrangebyscoreCommand(c, 1);
}

static void zlexcountCommand(rliteClient *c) {
	long count = 0;

	int retval = rl_zlexcount(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], UNSIGN(c->argv[3]), c->argvlen[3], &count);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		count = 0;
	}
	c->reply = createLongLongObject(count);
cleanup:
	return;
}

/* This command implements ZRANGEBYLEX, ZREVRANGEBYLEX. */
static void genericZrangebylexCommand(rliteClient *c, int reverse) {
	rl_zset_iterator *iterator;
	long offset = 0, limit = -1;

	if (c->argc > 4) {
		int remaining = c->argc - 4;
		int pos = 4;

		while (remaining) {
			if (remaining >= 3 && !strcasecmp(c->argv[pos],"limit")) {
				if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL) != RLITE_OK) ||
					(getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL) != RLITE_OK)) return;
				pos += 3; remaining -= 3;
			} else {
				c->reply = createErrorObject(RLITE_SYNTAXERR);
				return;
			}
		}
	}

	int retval = (reverse ? rl_zrevrangebylex : rl_zrangebylex)(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], UNSIGN(c->argv[3]), c->argvlen[3], offset, limit, &iterator);
	if (retval == RL_UNEXPECTED) {
		c->reply = createErrorObject(RLITE_INVALIDMINMAXERR);
	} else {
		addZsetIteratorReply(c, retval, iterator, 0);
	}
}

static void zrangebylexCommand(rliteClient *c) {
	genericZrangebylexCommand(c,0);
}

static void zrevrangebylexCommand(rliteClient *c) {
	genericZrangebylexCommand(c,1);
}

static void zscoreCommand(rliteClient *c) {
	double score;

	int retval = rl_zscore(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], &score);
	RLITE_SERVER_ERR(c, retval);

	if (retval == RL_FOUND) {
		c->reply = createDoubleObject(score);
	} else if (retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	}

cleanup:
	return;
}

static void zrankGenericCommand(rliteClient *c, int reverse) {
	long rank;

	int retval = (reverse ? rl_zrevrank : rl_zrank)(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], &rank);
	RLITE_SERVER_ERR(c, retval);

	if (retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	} else if (retval == RL_FOUND) {
		c->reply = createLongLongObject(rank);
	}
cleanup:
	return;
}

static void zrankCommand(rliteClient *c) {
	zrankGenericCommand(c, 0);
}

static void zrevrankCommand(rliteClient *c) {
	zrankGenericCommand(c, 1);
}

static void zcountCommand(rliteClient *c) {
	rl_zrangespec rlrange;
	long count;

	/* Parse the range arguments */
	if (zslParseRange(c->argv[2],c->argv[3],&rlrange) != RLITE_OK) {
		c->reply = createErrorObject("min or max is not a float");
		return;
	}

	int retval = rl_zcount(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &rlrange, &count);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		count = 0;
	}
	c->reply = createLongLongObject(count);
cleanup:
	return;
}

static void hsetGenericCommand(rliteClient *c, int update) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	unsigned char *field = UNSIGN(c->argv[2]);
	size_t fieldlen = c->argvlen[2];
	unsigned char *value = UNSIGN(c->argv[3]);
	size_t valuelen = c->argvlen[3];
	long added;

	int retval;
	retval = rl_hset(c->context->db, key, keylen, field, fieldlen, value, valuelen, &added, update);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(added);

cleanup:
	return;
}

static void hsetCommand(rliteClient *c) {
	hsetGenericCommand(c, 1);
}

static void hsetnxCommand(rliteClient *c) {
	hsetGenericCommand(c, 0);
}

static void hgetCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	unsigned char *field = UNSIGN(c->argv[2]);
	size_t fieldlen = c->argvlen[2];
	unsigned char *value = NULL;
	long valuelen;

	int retval;
	retval = rl_hget(c->context->db, key, keylen, field, fieldlen, &value, &valuelen);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	} else {
		c->reply = createStringObject((char *)value, valuelen);
	}

	rl_free(value);
cleanup:
	return;
}

static void hexistsCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	unsigned char *field = UNSIGN(c->argv[2]);
	size_t fieldlen = c->argvlen[2];

	int retval;
	retval = rl_hget(c->context->db, key, keylen, field, fieldlen, NULL, NULL);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(retval == RL_NOT_FOUND ? 0 : 1);
cleanup:
	return;
}

static void hmsetCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];

	int i, j, retval;
	if (c->argc % 2) {
		addReplyErrorFormat(c->context, RLITE_WRONGNUMBEROFARGUMENTS, c->argv[0]);
		return;
	}

	int fieldc = (c->argc - 2) / 2;
	unsigned char **fields = NULL;
	long *fieldslen = NULL;
	unsigned char **values = NULL;
	long *valueslen = NULL;

	fields = malloc(sizeof(unsigned char *) * fieldc);
	if (!fields) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	fieldslen = malloc(sizeof(long) * fieldc);
	if (!fieldslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	values = malloc(sizeof(unsigned char *) * fieldc);
	if (!values) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	valueslen = malloc(sizeof(long) * fieldc);
	if (!valueslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0, j = 2; i < fieldc; i++) {
		fields[i] = UNSIGN(c->argv[j]);
		fieldslen[i] = c->argvlen[j++];
		values[i] = UNSIGN(c->argv[j]);
		valueslen[i] = c->argvlen[j++];
	}
	retval = rl_hmset(c->context->db, key, keylen, fieldc, fields, fieldslen, values, valueslen);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createStatusObject(RLITE_STR_OK);
cleanup:
	free(fields);
	free(fieldslen);
	free(values);
	free(valueslen);
	return;
}

static void hdelCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	long delcount;

	int j, retval;
	// memberslen needs long, we have size_t (unsigned long)
	// it would be great not to need this
	long *memberslen = malloc(sizeof(long) * (c->argc - 2));
	if (!memberslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (j = 2; j < c->argc; j++) {
		memberslen[j - 2] = c->argvlen[j];
	}
	retval = rl_hdel(c->context->db, key, keylen, c->argc - 2, (unsigned char **)&c->argv[2], memberslen, &delcount);
	free(memberslen);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(delcount);
cleanup:
	return;
}

static void hlenCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	long len;
	int retval;

	retval = rl_hlen(c->context->db, key, keylen, &len);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(len);
cleanup:
	return;
}

static void hincrbyCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	int retval;
	long increment, newvalue;

	if ((getLongFromObjectOrReply(c, c->argv[3], &increment, NULL) != RLITE_OK)) return;

	retval = rl_hincrby(c->context->db, key, keylen, UNSIGN(c->argv[2]), c->argvlen[2], increment, &newvalue);
	if (retval == RL_NAN) {
		c->reply = createErrorObject("hash value is not an integer");
		goto cleanup;
	}
	else if (retval == RL_OVERFLOW) {
		c->reply = createErrorObject("increment or decrement would overflow");
		goto cleanup;
	}
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(newvalue);
cleanup:
	return;
}

static void hincrbyfloatCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	int retval;
	double increment, newvalue;

	if ((getDoubleFromObjectOrReply(c, c->argv[3], &increment, NULL) != RLITE_OK)) return;

	retval = rl_hincrbyfloat(c->context->db, key, keylen, UNSIGN(c->argv[2]), c->argvlen[2], increment, &newvalue);
	if (retval == RL_NAN) {
		c->reply = createErrorObject("hash value is not a float");
		goto cleanup;
	}
	RLITE_SERVER_ERR(c, retval);
	c->reply = createDoubleObject(newvalue);
cleanup:
	return;
}

static void addHashIteratorReply(rliteClient *c, int retval, rl_hash_iterator *iterator, int fields, int values)
{
	unsigned char *field, *value;
	long fieldlen, valuelen;
	long i = 0;

	c->reply = createReplyObject(RLITE_REPLY_ARRAY);
	if (retval == RL_NOT_FOUND) {
		c->reply->elements = 0;
		return;
	}
	c->reply->elements = iterator->size * (fields + values);
	c->reply->element = malloc(sizeof(rliteReply*) * c->reply->elements);
	while ((retval = rl_hash_iterator_next(iterator,
					fields ? &field : NULL, fields ? &fieldlen : NULL,
					values ? &value : NULL, values ? &valuelen : NULL
					)) == RL_OK) {
		if (fields) {
			c->reply->element[i] = createStringObject((char *)field, fieldlen);
			rl_free(field);
			i++;
		}
		if (values) {
			c->reply->element[i] = createStringObject((char *)value, valuelen);
			rl_free(value);
			i++;
		}
	}

	if (retval != RL_END) {
		__rliteSetError(c->context, RLITE_ERR, "Unexpected early end");
		goto cleanup;
	}
	iterator = NULL;
cleanup:
	if (iterator) {
		rl_hash_iterator_destroy(iterator);
	}
}

static void hgetallCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	rl_hash_iterator *iterator;
	int retval = rl_hgetall(c->context->db, &iterator, key, keylen);
	RLITE_SERVER_ERR(c, retval);
	addHashIteratorReply(c, retval, iterator, 1, 1);
cleanup:
	return;
}

static void hkeysCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	rl_hash_iterator *iterator;
	int retval = rl_hgetall(c->context->db, &iterator, key, keylen);
	RLITE_SERVER_ERR(c, retval);
	addHashIteratorReply(c, retval, iterator, 1, 0);
cleanup:
	return;
}

static void hvalsCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	rl_hash_iterator *iterator;
	int retval = rl_hgetall(c->context->db, &iterator, key, keylen);
	RLITE_SERVER_ERR(c, retval);
	addHashIteratorReply(c, retval, iterator, 0, 1);
cleanup:
	return;
}

static void hmgetCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];

	int i, fieldc = c->argc - 2;
	unsigned char **fields = NULL;
	long *fieldslen = NULL;
	unsigned char **values = NULL;
	long *valueslen = NULL;

	fields = malloc(sizeof(unsigned char *) * fieldc);
	if (!fields) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	fieldslen = malloc(sizeof(long) * fieldc);
	if (!fieldslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < fieldc; i++) {
		fields[i] = (unsigned char *)c->argv[2 + i];
		fieldslen[i] = (long)c->argvlen[2 + i];
	}

	int retval = rl_hmget(c->context->db, key, keylen, fieldc, fields, fieldslen, &values, &valueslen);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createReplyObject(RLITE_REPLY_ARRAY);
	c->reply->elements = fieldc;
	c->reply->element = malloc(sizeof(rliteReply*) * c->reply->elements);
	if (!c->reply->element) {
		free(c->reply);
		c->reply = NULL;
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}

	for (i = 0; i < fieldc; i++) {
		if (retval == RL_NOT_FOUND || valueslen[i] < 0) {
			c->reply->element[i] = createReplyObject(RLITE_REPLY_NIL);
		} else {
			c->reply->element[i] = createStringObject((char *)values[i], valueslen[i]);
			rl_free(values[i]);
		}
	}
	retval = RL_OK;
cleanup:
	free(fields);
	free(fieldslen);
	rl_free(values);
	rl_free(valueslen);
}

static void saddCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];

	int i, memberc = c->argc - 2;
	unsigned char **members = NULL;
	long *memberslen = NULL;
	long count;

	members = malloc(sizeof(unsigned char *) * memberc);
	if (!members) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	memberslen = malloc(sizeof(long) * memberc);
	if (!memberslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < memberc; i++) {
		members[i] = (unsigned char *)c->argv[2 + i];
		memberslen[i] = (long)c->argvlen[2 + i];
	}

	int retval = rl_sadd(c->context->db, key, keylen, memberc, members, memberslen, &count);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(count);
cleanup:
	free(members);
	free(memberslen);
}

static void scardCommand(rliteClient *c) {
	long card = 0;
	int retval = rl_scard(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &card);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(card);
cleanup:
	return;
}

static void sismemberCommand(rliteClient *c) {
	int retval = rl_sismember(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2]);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(retval == RL_FOUND ? 1 : 0);
cleanup:
	return;
}

static void smoveCommand(rliteClient *c) {
	int retval = rl_smove(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], UNSIGN(c->argv[3]), c->argvlen[3]);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(retval == RL_OK ? 1 : 0);
cleanup:
	return;
}

static void spopCommand(rliteClient *c) {
	unsigned char *member;
	long memberlen;
	int retval = rl_spop(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &member, &memberlen);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	} else if (retval == RL_OK) {
		c->reply = createStringObject((char *)member, memberlen);
		rl_free(member);
	}
cleanup:
	return;
}

static void srandmemberCommand(rliteClient *c) {
	unsigned char **members = NULL;
	long *memberslen = NULL;
	long count, i;
	int repeat;

	if (c->argc == 2) {
		count = 1;
		repeat = 0;
	} else {
		if ((getLongFromObjectOrReply(c, c->argv[2], &count, NULL) != RLITE_OK)) return;
		repeat = count < 0 ? 1 : 0;
		count = count >= 0 ? count : -count;
	}

	int retval = rl_srandmembers(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], repeat, &count, &members, &memberslen);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_OK && count > 0) {
		if (c->argc == 2) {
			c->reply = createStringObject((char *)members[0], memberslen[0]);
		} else {
			c->reply = createReplyObject(RLITE_REPLY_ARRAY);
			c->reply->elements = count;
			c->reply->element = malloc(sizeof(rliteReply*) * count);
			for (i = 0; i < count; i++) {
				c->reply->element[i] = createStringObject((char *)members[i], memberslen[i]);
			}
		}
		for (i = 0; i < count; i++) {
			rl_free(members[i]);
		}
	} else {
		c->reply = createReplyObject(RLITE_REPLY_ARRAY);
		c->reply->elements = 0;
	}
cleanup:
	rl_free(members);
	rl_free(memberslen);
	return;
}

static void sremCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];

	int i, memberc = c->argc - 2;
	unsigned char **members = NULL;
	long *memberslen = NULL;
	long count;

	members = malloc(sizeof(unsigned char *) * memberc);
	if (!members) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	memberslen = malloc(sizeof(long) * memberc);
	if (!memberslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < memberc; i++) {
		members[i] = (unsigned char *)c->argv[2 + i];
		memberslen[i] = (long)c->argvlen[2 + i];
	}

	int retval = rl_srem(c->context->db, key, keylen, memberc, members, memberslen, &count);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(count);
cleanup:
	free(members);
	free(memberslen);
}

static void sinterCommand(rliteClient *c) {
	int keyc = c->argc - 1, i, retval;
	unsigned char **keys = NULL, **members = NULL;
	long *keyslen = NULL, j, membersc, *memberslen = NULL;

	keys = malloc(sizeof(unsigned char *) * keyc);
	if (!keys) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	keyslen = malloc(sizeof(long) * keyc);
	if (!keyslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < keyc; i++) {
		keys[i] = UNSIGN(c->argv[1 + i]);
		keyslen[i] = c->argvlen[1 + i];
	}
	retval = rl_sinter(c->context->db, keyc, keys, keyslen, &membersc, &members, &memberslen);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createReplyObject(RLITE_REPLY_ARRAY);
	c->reply->elements = membersc;
	if (membersc > 0) {
		c->reply->element = malloc(sizeof(rliteReply*) * membersc);
		if (!c->reply->element) {
			free(c->reply);
			__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
			goto cleanup;
		}
		for (j = 0; j < membersc; j++) {
			c->reply->element[j] = createStringObject((char *)members[j], memberslen[j]);
		}
	}
cleanup:
	if (members) {
		for (j = 0; j < membersc; j++) {
			rl_free(members[j]);
		}
		rl_free(members);
		rl_free(memberslen);
	}
	free(keys);
	free(keyslen);
	return;
}

static void sinterstoreCommand(rliteClient *c) {
	int keyc = c->argc - 2, i, retval;
	unsigned char **keys = NULL;
	long *keyslen = NULL, membersc;
	unsigned char *target = UNSIGN(c->argv[1]);
	long targetlen = c->argvlen[1];

	keys = malloc(sizeof(unsigned char *) * keyc);
	if (!keys) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	keyslen = malloc(sizeof(long) * keyc);
	if (!keyslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < keyc; i++) {
		keys[i] = UNSIGN(c->argv[2 + i]);
		keyslen[i] = c->argvlen[2 + i];
	}
	retval = rl_sinterstore(c->context->db, target, targetlen, keyc, keys, keyslen, &membersc);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(membersc);
cleanup:
	free(keys);
	free(keyslen);
	return;
}

static void sunionCommand(rliteClient *c) {
	int keyc = c->argc - 1, i, retval;
	unsigned char **keys = NULL, **members = NULL;
	long *keyslen = NULL, j, membersc, *memberslen = NULL;

	keys = malloc(sizeof(unsigned char *) * keyc);
	if (!keys) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	keyslen = malloc(sizeof(long) * keyc);
	if (!keyslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < keyc; i++) {
		keys[i] = UNSIGN(c->argv[1 + i]);
		keyslen[i] = c->argvlen[1 + i];
	}
	retval = rl_sunion(c->context->db, keyc, keys, keyslen, &membersc, &members, &memberslen);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createReplyObject(RLITE_REPLY_ARRAY);
	c->reply->elements = membersc;
	if (membersc > 0) {
		c->reply->element = malloc(sizeof(rliteReply*) * membersc);
		if (!c->reply->element) {
			free(c->reply);
			__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
			goto cleanup;
		}
		for (j = 0; j < membersc; j++) {
			c->reply->element[j] = createStringObject((char *)members[j], memberslen[j]);
		}
	}
cleanup:
	if (members) {
		for (j = 0; j < membersc; j++) {
			rl_free(members[j]);
		}
		rl_free(members);
		rl_free(memberslen);
	}
	free(keys);
	free(keyslen);
	return;
}

static void sunionstoreCommand(rliteClient *c) {
	int keyc = c->argc - 2, i, retval;
	unsigned char **keys = NULL;
	long *keyslen = NULL, membersc;
	unsigned char *target = UNSIGN(c->argv[1]);
	long targetlen = c->argvlen[1];

	keys = malloc(sizeof(unsigned char *) * keyc);
	if (!keys) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	keyslen = malloc(sizeof(long) * keyc);
	if (!keyslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < keyc; i++) {
		keys[i] = UNSIGN(c->argv[2 + i]);
		keyslen[i] = c->argvlen[2 + i];
	}
	retval = rl_sunionstore(c->context->db, target, targetlen, keyc, keys, keyslen, &membersc);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(membersc);
cleanup:
	free(keys);
	free(keyslen);
	return;
}

static void sdiffCommand(rliteClient *c) {
	int keyc = c->argc - 1, i, retval;
	unsigned char **keys = NULL, **members = NULL;
	long *keyslen = NULL, j, membersc, *memberslen = NULL;

	keys = malloc(sizeof(unsigned char *) * keyc);
	if (!keys) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	keyslen = malloc(sizeof(long) * keyc);
	if (!keyslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < keyc; i++) {
		keys[i] = UNSIGN(c->argv[1 + i]);
		keyslen[i] = c->argvlen[1 + i];
	}
	retval = rl_sdiff(c->context->db, keyc, keys, keyslen, &membersc, &members, &memberslen);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createReplyObject(RLITE_REPLY_ARRAY);
	c->reply->elements = membersc;
	if (membersc > 0) {
		c->reply->element = malloc(sizeof(rliteReply*) * membersc);
		if (!c->reply->element) {
			free(c->reply);
			__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
			goto cleanup;
		}
		for (j = 0; j < membersc; j++) {
			c->reply->element[j] = createStringObject((char *)members[j], memberslen[j]);
		}
	}
cleanup:
	if (members) {
		for (j = 0; j < membersc; j++) {
			rl_free(members[j]);
		}
		rl_free(members);
		rl_free(memberslen);
	}
	free(keys);
	free(keyslen);
	return;
}

static void sdiffstoreCommand(rliteClient *c) {
	int keyc = c->argc - 2, i, retval;
	unsigned char **keys = NULL;
	long *keyslen = NULL, membersc;
	unsigned char *target = UNSIGN(c->argv[1]);
	long targetlen = c->argvlen[1];

	keys = malloc(sizeof(unsigned char *) * keyc);
	if (!keys) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	keyslen = malloc(sizeof(long) * keyc);
	if (!keyslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < keyc; i++) {
		keys[i] = UNSIGN(c->argv[2 + i]);
		keyslen[i] = c->argvlen[2 + i];
	}
	retval = rl_sdiffstore(c->context->db, target, targetlen, keyc, keys, keyslen, &membersc);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(membersc);
cleanup:
	free(keys);
	free(keyslen);
	return;
}

static void pushGenericCommand(rliteClient *c, int create, int left) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];

	int i, valuec = c->argc - 2;
	unsigned char **values = NULL;
	long *valueslen = NULL;
	long count;

	values = malloc(sizeof(unsigned char *) * valuec);
	if (!values) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	valueslen = malloc(sizeof(long) * valuec);
	if (!valueslen) {
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}
	for (i = 0; i < valuec; i++) {
		values[i] = (unsigned char *)c->argv[2 + i];
		valueslen[i] = (long)c->argvlen[2 + i];
	}

	int retval = rl_push(c->context->db, key, keylen, create, left, valuec, values, valueslen, &count);
	if (retval == RL_NOT_FOUND) {
		c->reply = createLongLongObject(0);
	} else {
		RLITE_SERVER_ERR(c, retval);
		c->reply = createLongLongObject(count);
	}
cleanup:
	free(values);
	free(valueslen);
}

static void lpushCommand(rliteClient *c) {
	pushGenericCommand(c, 1, 1);
}

static void lpushxCommand(rliteClient *c) {
	pushGenericCommand(c, 0, 1);
}

static void rpushCommand(rliteClient *c) {
	pushGenericCommand(c, 1, 0);
}

static void rpushxCommand(rliteClient *c) {
	pushGenericCommand(c, 0, 0);
}

static void llenCommand(rliteClient *c) {
	long len = 0;
	int retval = rl_llen(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &len);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(len);
cleanup:
	return;
}

static void popGenericCommand(rliteClient *c, int left) {
	unsigned char *value;
	long valuelen;
	int retval = rl_pop(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &value, &valuelen, left);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	} else if (retval == RL_OK) {
		c->reply = createStringObject((char *)value, valuelen);
		rl_free(value);
	}
cleanup:
	return;
}

static void rpopCommand(rliteClient *c) {
	popGenericCommand(c, 0);
}

static void lpopCommand(rliteClient *c) {
	popGenericCommand(c, 1);
}

static void lindexCommand(rliteClient *c) {
	long index;
	unsigned char *value;
	long valuelen;

	if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != RLITE_OK))
		return;

	int retval = rl_lindex(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], index, &value, &valuelen);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_INVALID_PARAMETERS || retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	} else if (retval == RL_OK) {
		c->reply = createStringObject((char *)value, valuelen);
		rl_free(value);
	}
cleanup:
	return;
}

static void linsertCommand(rliteClient *c) {
	int after;
	if (strcasecmp(c->argv[2], "after") == 0) {
		after = 1;
	} else if (strcasecmp(c->argv[2], "before") == 0) {
		after = 0;
	} else {
		c->reply = createErrorObject(RLITE_SYNTAXERR);
		return;
	}

	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];

	long size;
	int retval = rl_linsert(c->context->db, key, keylen, after, UNSIGN(c->argv[3]), c->argvlen[3], UNSIGN(c->argv[4]), c->argvlen[4], &size);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_OK) {
		c->reply = createLongLongObject(size);
	} else {
		c->reply = createLongLongObject(-1);
	}
cleanup:
	return;
}

static void lrangeCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];

	unsigned char **values = NULL;
	long size = 0, *valueslen = NULL;

	long start, stop, i;

	if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != RLITE_OK) ||
		(getLongFromObjectOrReply(c, c->argv[3], &stop, NULL) != RLITE_OK)) return;

	int retval = rl_lrange(c->context->db, key, keylen, start, stop, &size, &values, &valueslen);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createReplyObject(RLITE_REPLY_ARRAY);
	if (retval == RL_NOT_FOUND) {
		c->reply->elements = 0;
		return;
	}
	c->reply->elements = size;
	c->reply->element = malloc(sizeof(rliteReply*) * c->reply->elements);
	for (i = 0; i < size; i++) {
		c->reply->element[i] = createStringObject((char *)values[i], valueslen[i]);
	}
cleanup:
	if (values) {
		for (i = 0; i < size; i++) {
			rl_free(values[i]);
		}
		rl_free(values);
		rl_free(valueslen);
	}
}

static void lremCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	long count, maxcount, resultcount;
	int direction;

	if ((getLongFromObjectOrReply(c, c->argv[2], &count, NULL) != RLITE_OK)) return;

	if (count >= 0) {
		direction = 1;
		maxcount = count;
	} else {
		direction = -1;
		maxcount = -count;
	}
	int retval = rl_lrem(c->context->db, key, keylen, direction,  maxcount, UNSIGN(c->argv[3]), c->argvlen[3], &resultcount);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createLongLongObject(resultcount);
cleanup:
	return;
}

static void lsetCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	long index;

	if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != RLITE_OK)) return;

	int retval = rl_lset(c->context->db, key, keylen, index, UNSIGN(c->argv[3]), c->argvlen[3]);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		c->reply = createErrorObject(RLITE_NOKEYERR);
	} else if (retval == RL_INVALID_PARAMETERS) {
		c->reply = createErrorObject(RLITE_OUTOFRANGEERR);
	} else if (retval == RL_OK) {
		c->reply = createStatusObject(RLITE_STR_OK);
	}
cleanup:
	return;
}

static void ltrimCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	long start, stop;

	if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != RLITE_OK) ||
		(getLongFromObjectOrReply(c, c->argv[3], &stop, NULL) != RLITE_OK)) return;

	int retval = rl_ltrim(c->context->db, key, keylen, start, stop);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createStatusObject(RLITE_STR_OK);
cleanup:
	return;
}

static void rpoplpushCommand(rliteClient *c) {
	unsigned char *value;
	long valuelen;
	int retval = rl_pop(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], &value, &valuelen, 0);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
		goto cleanup;
	}
	if (retval != RL_OK) {
		goto cleanup;
	}
	retval = rl_push(c->context->db, UNSIGN(c->argv[2]), c->argvlen[2], 1, 1, 1, &value, &valuelen, NULL);
	RLITE_SERVER_ERR(c, retval);

	c->reply = createStringObject((char *)value, valuelen);
	rl_free(value);
cleanup:
	return;
}

#define REDIS_SET_NO_FLAGS 0
#define REDIS_SET_NX (1<<0)	 /* Set if key not exists. */
#define REDIS_SET_XX (1<<1)	 /* Set if key exists. */

void setGenericCommand(rliteClient *c, int flags, const unsigned char *key, long keylen, unsigned char *value, long valuelen, long long expire) {
	int retval;
	long long milliseconds = 0; /* initialized to avoid any harmness warning */

	if (expire) {
		if (expire <= 0) {
			addReplyErrorFormat(c->context, "invalid expire time in %s", c->argv[0]);
			return;
		}

		milliseconds = rl_mstime() + expire;
	}

	if ((flags & REDIS_SET_NX) || (flags & REDIS_SET_XX)) {
		retval = rl_key_get(c->context->db, key, keylen, NULL, NULL, NULL, NULL);
		if (((flags & REDIS_SET_NX) && retval == RL_FOUND) ||
				((flags & REDIS_SET_XX) && retval == RL_NOT_FOUND)) {
			c->reply = createReplyObject(RLITE_REPLY_NIL);
			return;
		}
	}

	retval = rl_set(c->context->db, key, keylen, value, valuelen, 0, milliseconds);
	RLITE_SERVER_ERR(c, retval);
	c->reply = createStatusObject(RLITE_STR_OK);
cleanup:
	return;
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
void setCommand(rliteClient *c) {
	const unsigned char *key = UNSIGN(c->argv[1]);
	size_t keylen = c->argvlen[1];
	int j;
	long long expire = 0, next;
	int flags = REDIS_SET_NO_FLAGS;

	for (j = 3; j < c->argc; j++) {
		char *a = c->argv[j];

		next = 0;
		if (j < c->argc - 1 && getLongLongFromObject(c->argv[j + 1], &next) != RLITE_OK) {
			next = 0;
		}

		if ((a[0] == 'n' || a[0] == 'N') &&
			(a[1] == 'x' || a[1] == 'X') && a[2] == '\0') {
			flags |= REDIS_SET_NX;
		} else if ((a[0] == 'x' || a[0] == 'X') &&
				   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0') {
			flags |= REDIS_SET_XX;
		} else if ((a[0] == 'e' || a[0] == 'E') &&
				   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' && next) {
			expire = next * 1000;
			j++;
		} else if ((a[0] == 'p' || a[0] == 'P') &&
				   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' && next) {
			expire = next;
			j++;
		} else {
			c->reply = createErrorObject(RLITE_SYNTAXERR);
			return;
		}
	}

	setGenericCommand(c, flags, key, keylen, UNSIGN(c->argv[2]), c->argvlen[2], expire);
}

void setnxCommand(rliteClient *c) {
	rliteReply *reply;
	setGenericCommand(c, REDIS_SET_NX, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], 0);
	reply = c->reply;
	if (reply->type == RLITE_REPLY_NIL) {
		c->reply = createLongLongObject(0);
	} else {
		c->reply = createLongLongObject(1);
	}
	rliteFreeReplyObject(reply);
}

void setexCommand(rliteClient *c) {
	long long expire;
	if (getLongLongFromObject(c->argv[3], &expire) != RLITE_OK) {
		c->reply = createErrorObject(RLITE_SYNTAXERR);
		return;
	}
	setGenericCommand(c, REDIS_SET_NO_FLAGS, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], expire * 1000);
}

void psetexCommand(rliteClient *c) {
	long long expire;
	if (getLongLongFromObject(c->argv[3], &expire) != RLITE_OK) {
		c->reply = createErrorObject(RLITE_SYNTAXERR);
		return;
	}
	setGenericCommand(c, REDIS_SET_NO_FLAGS, UNSIGN(c->argv[1]), c->argvlen[1], UNSIGN(c->argv[2]), c->argvlen[2], expire);
}

static void getCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	long keylen = c->argvlen[1];
	unsigned char *value = NULL;
	long valuelen;

	int retval;
	retval = rl_get(c->context->db, key, keylen, &value, &valuelen);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	} else {
		c->reply = createStringObject((char *)value, valuelen);
	}

	rl_free(value);
cleanup:
	return;
}

static void appendCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	long keylen = c->argvlen[1];
	unsigned char *value = UNSIGN(c->argv[2]);
	long valuelen = c->argvlen[2];
	long newlen;

	int retval;
	retval = rl_append(c->context->db, key, keylen, value, valuelen, &newlen);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_OK) {
		c->reply = createLongLongObject(newlen);
	}
cleanup:
	return;
}

static void getsetCommand(rliteClient *c) {
	unsigned char *key = UNSIGN(c->argv[1]);
	long keylen = c->argvlen[1];
	unsigned char *value = UNSIGN(c->argv[2]);
	long valuelen = c->argvlen[2];
	unsigned char *prevvalue = NULL;
	long prevvaluelen;

	int retval;
	retval = rl_get(c->context->db, key, keylen, &prevvalue, &prevvaluelen);
	RLITE_SERVER_ERR(c, retval);
	if (retval == RL_NOT_FOUND) {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	} else {
		c->reply = createStringObject((char *)prevvalue, prevvaluelen);
	}
	rl_free(prevvalue);

	retval = rl_set(c->context->db, key, keylen, value, valuelen, 0, 0);
	RLITE_SERVER_ERR(c, retval);
cleanup:
	return;
}

static void mgetCommand(rliteClient *c) {
	int retval, i = 0, keyc = c->argc - 1;
	unsigned char *key;
	long keylen;
	unsigned char *value;
	long valuelen;

	c->reply = createReplyObject(RLITE_REPLY_ARRAY);
	c->reply->elements = keyc;
	c->reply->element = malloc(sizeof(rliteReply*) * c->reply->elements);
	if (!c->reply->element) {
		free(c->reply);
		c->reply = NULL;
		__rliteSetError(c->context, RLITE_ERR_OOM, "Out of memory");
		goto cleanup;
	}

	for (i = 0; i < keyc; i++) {
		key = (unsigned char *)c->argv[1 + i];
		keylen = (long)c->argvlen[1 + i];
		int retval = rl_get(c->context->db, key, keylen, &value, &valuelen);
		// return nil for keys that are not strings
		if (retval == RL_WRONG_TYPE) {
			retval = RL_NOT_FOUND;
		}
		RLITE_SERVER_ERR(c, retval);
		if (retval == RL_NOT_FOUND) {
			c->reply->element[i] = createReplyObject(RLITE_REPLY_NIL);
		} else {
			c->reply->element[i] = createStringObject((char *)value, valuelen);
			rl_free(value);
		}
	}
	retval = RL_OK;
cleanup:
	if (retval != RL_OK) {
		for (; i >= 0; i--) {
			free(c->reply->element[i]);
		}
		free(c->reply);
		c->reply = NULL;
	}
	return;
}

static void msetCommand(rliteClient *c) {
	int retval, i = 0, keyc = c->argc - 1;
	unsigned char *key;
	long keylen;
	unsigned char *value;
	long valuelen;

	if (c->argc % 2 == 0) {
		c->reply = createErrorObject(RLITE_SYNTAXERR);
		return;
	}

	for (i = 1; i < keyc; i += 2) {
		key = (unsigned char *)c->argv[i];
		keylen = (long)c->argvlen[i];
		value = (unsigned char *)c->argv[i + 1];
		valuelen = (long)c->argvlen[i + 1];
		retval = rl_set(c->context->db, key, keylen, value, valuelen, 0, 0);
		RLITE_SERVER_ERR(c, retval);
	}
	c->reply = createStatusObject(RLITE_STR_OK);
	retval = RL_OK;
cleanup:
	return;
}

static void delCommand(rliteClient *c) {
	int deleted = 0, j, retval;

	for (j = 1; j < c->argc; j++) {
		retval = rl_key_delete_with_value(c->context->db, UNSIGN(c->argv[j]), c->argvlen[j]);
		if (retval == RL_OK) {
			deleted++;
		}
	}
	c->reply = createLongLongObject(deleted);
}

static void existsCommand(rliteClient *c) {
	int retval = rl_key_get(c->context->db, UNSIGN(c->argv[1]), c->argvlen[1], NULL, NULL, NULL, NULL);
	c->reply = createLongLongObject(retval == RL_FOUND ? 1 : 0);
}

static void getKeyEncoding(rliteClient *c, char *encoding, unsigned char *key, long keylen)
{
	unsigned char type;
	encoding[0] = 0;
	if (rl_key_get(c->context->db, UNSIGN(key), keylen, &type, NULL, NULL, NULL)) {
		if (type == RL_TYPE_ZSET) {
			const char *enc = c->context->debugSkiplist ? "skiplist" : "ziplist";
			memcpy(encoding, enc, (strlen(enc) + 1) * sizeof(char));
		}
		else if (type == RL_TYPE_HASH) {
			unsigned char *key = UNSIGN(c->argv[2]), *value = NULL;
			long keylen = c->argvlen[2];
			long len, valuelen;
			rl_hash_iterator *iterator = NULL;
			int retval = rl_hlen(c->context->db, key, keylen, &len);
			int hashtable = c->context->hashtableLimitEntries < (size_t)len;
			if (!hashtable) {
				int retval = rl_hgetall(c->context->db, &iterator, key, keylen);
				RLITE_SERVER_ERR(c, retval);
				while ((retval = rl_hash_iterator_next(iterator, NULL, NULL, &value, &valuelen)) == RL_OK) {
					rl_free(value);
					if ((size_t)valuelen > c->context->hashtableLimitValue) {
						hashtable = 1;
						rl_hash_iterator_destroy(iterator);
						retval = RL_END;
						break;
					}
				}
				if (retval != RL_END) {
					goto cleanup;
				}
			}
			RLITE_SERVER_ERR(c, retval);
			const char *enc = hashtable ? "hashtable" : "ziplist";
			memcpy(encoding, enc, (strlen(enc) + 1) * sizeof(char));
		}
		else if (type == RL_TYPE_SET) {
			unsigned char *key = UNSIGN(c->argv[2]), *value = NULL;
			long keylen = c->argvlen[2];
			long len, valuelen;
			rl_set_iterator *iterator = NULL;
			int retval = rl_scard(c->context->db, key, keylen, &len);
			RLITE_SERVER_ERR(c, retval);
			int hashtable = 512 < (size_t)len;
			if (!hashtable) {
				char o[40];
				int retval = rl_smembers(c->context->db, &iterator, key, keylen);
				RLITE_SERVER_ERR(c, retval);
				while ((retval = rl_set_iterator_next(iterator, &value, &valuelen)) == RL_OK) {
					if (valuelen > 38) {
						hashtable = 1;
						rl_free(value);
						rl_set_iterator_destroy(iterator);
						retval = RL_END;
						break;
					}
					memcpy(o, value, sizeof(char) * valuelen);
					o[valuelen] = 0;
					if (getLongLongFromObject(o, NULL) != RLITE_OK) {
						hashtable = 1;
						rl_free(value);
						rl_set_iterator_destroy(iterator);
						retval = RL_END;
						break;
					}
					rl_free(value);
				}
				if (retval != RL_END) {
					goto cleanup;
				}
			}
			const char *enc = hashtable ? "hashtable" : "intset";
			memcpy(encoding, enc, (strlen(enc) + 1) * sizeof(char));
		}
		else if (type == RL_TYPE_LIST) {
			unsigned char **values;
			long *valueslen, size;
			unsigned char *key = UNSIGN(c->argv[2]);
			long keylen = c->argvlen[2];
			long len, i;
			int retval = rl_llen(c->context->db, key, keylen, &len);
			RLITE_SERVER_ERR(c, retval);
			int linkedlist = 256 < (size_t)len;
			if (!linkedlist) {
				int retval = rl_lrange(c->context->db, key, keylen, 0, -1, &size, &values, &valueslen);
				RLITE_SERVER_ERR(c, retval);
				for (i = 0; i < size; i++) {
					linkedlist = linkedlist || (valueslen[i] > 16);
					rl_free(values[i]);
				}
				rl_free(values);
				rl_free(valueslen);
			}
			const char *enc = linkedlist ? "linkedlist" : "ziplist";
			memcpy(encoding, enc, (strlen(enc) + 1) * sizeof(char));
		}
	}
cleanup:
	return;
}

static void debugCommand(rliteClient *c) {
	if (!strcasecmp(c->argv[1],"segfault")) {
		*((char*)-1) = 'x';
	} else if (!strcasecmp(c->argv[1],"oom")) {
		void *ptr = malloc(ULONG_MAX); /* Should trigger an out of memory. */
		free(ptr);
		c->reply = createStatusObject(RLITE_STR_OK);
	} else if (!strcasecmp(c->argv[1],"assert")) {
		// TODO
		c->reply = createErrorObject("Not implemented");
	} else if (!strcasecmp(c->argv[1],"reload")) {
		c->reply = createStatusObject(RLITE_STR_OK);
	} else if (!strcasecmp(c->argv[1],"loadaof")) {
		c->reply = createStatusObject(RLITE_STR_OK);
	} else if (!strcasecmp(c->argv[1],"object") && c->argc == 3) {
		char encoding[100];
		if (rl_key_get(c->context->db, UNSIGN(c->argv[2]), c->argvlen[2], NULL, NULL, NULL, NULL)) {
			getKeyEncoding(c, encoding, UNSIGN(c->argv[2]), c->argvlen[2]);
			addReplyStatusFormat(c->context,
				"Value at:0xfaceadd refcount:1 "
				"encoding:%s serializedlength:0 "
				"lru:0 lru_seconds_idle:0", encoding);
		}
	} else if (!strcasecmp(c->argv[1],"sdslen") && c->argc == 3) {
		// TODO
		c->reply = createErrorObject("Not implemented");
	} else if (!strcasecmp(c->argv[1],"populate") &&
			   (c->argc == 3 || c->argc == 4)) {
		c->reply = createErrorObject("Not implemented");
	} else if (!strcasecmp(c->argv[1],"digest") && c->argc == 2) {
		c->reply = createErrorObject("Not implemented");
	} else if (!strcasecmp(c->argv[1],"sleep") && c->argc == 3) {
		double dtime = strtod(c->argv[2], NULL);
		long long utime = dtime*1000000;
		struct timespec tv;

		tv.tv_sec = utime / 1000000;
		tv.tv_nsec = (utime % 1000000) * 1000;
		nanosleep(&tv, NULL);
		c->reply = createStatusObject(RLITE_STR_OK);
	} else if (!strcasecmp(c->argv[1],"set-active-expire") &&
			   c->argc == 3)
	{
		c->reply = createErrorObject("Not implemented");
	} else if (!strcasecmp(c->argv[1],"error") && c->argc == 3) {
		c->reply = createStringObject(c->argv[2], c->argvlen[2]);
	} else {
		c->reply = createStringObject(c->argv[2], c->argvlen[2]);
		addReplyErrorFormat(c->context, "Unknown DEBUG subcommand or wrong number of arguments for '%s'",
			(char*)c->argv[1]);
	}
}

static void objectCommand(rliteClient *c) {
	if (!strcasecmp(c->argv[1],"encoding")) {
		char encoding[100];
		getKeyEncoding(c, encoding, UNSIGN(c->argv[2]), c->argvlen[2]);
		c->reply = createCStringObject(encoding);
	} else {
		c->reply = createReplyObject(RLITE_REPLY_NIL);
	}
}

struct rliteCommand rliteCommandTable[] = {
	{"get",getCommand,2,"rF",0,1,1,1,0,0},
	{"set",setCommand,-3,"wm",0,1,1,1,0,0},
	{"setnx",setnxCommand,3,"wmF",0,1,1,1,0,0},
	{"setex",setexCommand,4,"wm",0,1,1,1,0,0},
	{"psetex",psetexCommand,4,"wm",0,1,1,1,0,0},
	{"append",appendCommand,3,"wm",0,1,1,1,0,0},
	// {"strlen",strlenCommand,2,"rF",0,NULL,1,1,1,0,0},
	{"del",delCommand,-2,"w",0,1,-1,1,0,0},
	{"exists",existsCommand,2,"rF",0,1,1,1,0,0},
	// {"setbit",setbitCommand,4,"wm",0,NULL,1,1,1,0,0},
	// {"getbit",getbitCommand,3,"rF",0,NULL,1,1,1,0,0},
	// {"setrange",setrangeCommand,4,"wm",0,NULL,1,1,1,0,0},
	// {"getrange",getrangeCommand,4,"r",0,NULL,1,1,1,0,0},
	// {"substr",getrangeCommand,4,"r",0,NULL,1,1,1,0,0},
	// {"incr",incrCommand,2,"wmF",0,NULL,1,1,1,0,0},
	// {"decr",decrCommand,2,"wmF",0,NULL,1,1,1,0,0},
	{"mget",mgetCommand,-2,"r",0,1,-1,1,0,0},
	{"rpush",rpushCommand,-3,"wmF",0,1,1,1,0,0},
	{"lpush",lpushCommand,-3,"wmF",0,1,1,1,0,0},
	{"rpushx",rpushxCommand,3,"wmF",0,1,1,1,0,0},
	{"lpushx",lpushxCommand,3,"wmF",0,1,1,1,0,0},
	{"linsert",linsertCommand,5,"wm",0,1,1,1,0,0},
	{"rpop",rpopCommand,2,"wF",0,1,1,1,0,0},
	{"lpop",lpopCommand,2,"wF",0,1,1,1,0,0},
	// {"brpop",brpopCommand,-3,"ws",0,NULL,1,1,1,0,0},
	// {"brpoplpush",brpoplpushCommand,4,"wms",0,NULL,1,2,1,0,0},
	// {"blpop",blpopCommand,-3,"ws",0,NULL,1,-2,1,0,0},
	{"llen",llenCommand,2,"rF",0,1,1,1,0,0},
	{"lindex",lindexCommand,3,"r",0,1,1,1,0,0},
	{"lset",lsetCommand,4,"wm",0,1,1,1,0,0},
	{"lrange",lrangeCommand,4,"r",0,1,1,1,0,0},
	{"ltrim",ltrimCommand,4,"w",0,1,1,1,0,0},
	{"lrem",lremCommand,4,"w",0,1,1,1,0,0},
	{"rpoplpush",rpoplpushCommand,3,"wm",0,1,2,1,0,0},
	{"sadd",saddCommand,-3,"wmF",0,1,1,1,0,0},
	{"srem",sremCommand,-3,"wF",0,1,1,1,0,0},
	{"smove",smoveCommand,4,"wF",0,1,2,1,0,0},
	{"sismember",sismemberCommand,3,"rF",0,1,1,1,0,0},
	{"scard",scardCommand,2,"rF",0,1,1,1,0,0},
	{"spop",spopCommand,2,"wRsF",0,1,1,1,0,0},
	{"srandmember",srandmemberCommand,-2,"rR",0,1,1,1,0,0},
	{"sinter",sinterCommand,-2,"rS",0,1,-1,1,0,0},
	{"sinterstore",sinterstoreCommand,-3,"wm",0,1,-1,1,0,0},
	{"sunion",sunionCommand,-2,"rS",0,1,-1,1,0,0},
	{"sunionstore",sunionstoreCommand,-3,"wm",0,1,-1,1,0,0},
	{"sdiff",sdiffCommand,-2,"rS",0,1,-1,1,0,0},
	{"sdiffstore",sdiffstoreCommand,-3,"wm",0,1,-1,1,0,0},
	{"smembers",sinterCommand,2,"rS",0,1,1,1,0,0},
	// {"sscan",sscanCommand,-3,"rR",0,NULL,1,1,1,0,0},
	{"zadd",zaddCommand,-4,"wmF",0,1,1,1,0,0},
	{"zincrby",zincrbyCommand,4,"wmF",0,1,1,1,0,0},
	{"zrem",zremCommand,-3,"wF",0,1,1,1,0,0},
	{"zremrangebyscore",zremrangebyscoreCommand,4,"w",0,1,1,1,0,0},
	{"zremrangebyrank",zremrangebyrankCommand,4,"w",0,1,1,1,0,0},
	{"zremrangebylex",zremrangebylexCommand,4,"w",0,1,1,1,0,0},
	{"zunionstore",zunionstoreCommand,-4,"wm",0,0,0,0,0,0},
	{"zinterstore",zinterstoreCommand,-4,"wm",0,0,0,0,0,0},
	{"zrange",zrangeCommand,-4,"r",0,1,1,1,0,0},
	{"zrangebyscore",zrangebyscoreCommand,-4,"r",0,1,1,1,0,0},
	{"zrevrangebyscore",zrevrangebyscoreCommand,-4,"r",0,1,1,1,0,0},
	{"zrangebylex",zrangebylexCommand,-4,"r",0,1,1,1,0,0},
	{"zrevrangebylex",zrevrangebylexCommand,-4,"r",0,1,1,1,0,0},
	{"zcount",zcountCommand,4,"rF",0,1,1,1,0,0},
	{"zlexcount",zlexcountCommand,4,"rF",0,1,1,1,0,0},
	{"zrevrange",zrevrangeCommand,-4,"r",0,1,1,1,0,0},
	{"zcard",zcardCommand,2,"rF",0,1,1,1,0,0},
	{"zscore",zscoreCommand,3,"rF",0,1,1,1,0,0},
	{"zrank",zrankCommand,3,"rF",0,1,1,1,0,0},
	{"zrevrank",zrevrankCommand,3,"rF",0,1,1,1,0,0},
	// {"zscan",zscanCommand,-3,"rR",0,NULL,1,1,1,0,0},
	{"hset",hsetCommand,4,"wmF",0,1,1,1,0,0},
	{"hsetnx",hsetnxCommand,4,"wmF",0,1,1,1,0,0},
	{"hget",hgetCommand,3,"rF",0,1,1,1,0,0},
	{"hmset",hmsetCommand,-4,"wm",0,1,1,1,0,0},
	{"hmget",hmgetCommand,-3,"r",0,1,1,1,0,0},
	{"hincrby",hincrbyCommand,4,"wmF",0,1,1,1,0,0},
	{"hincrbyfloat",hincrbyfloatCommand,4,"wmF",0,1,1,1,0,0},
	{"hdel",hdelCommand,-3,"wF",0,1,1,1,0,0},
	{"hlen",hlenCommand,2,"rF",0,1,1,1,0,0},
	{"hkeys",hkeysCommand,2,"rS",0,1,1,1,0,0},
	{"hvals",hvalsCommand,2,"rS",0,1,1,1,0,0},
	{"hgetall",hgetallCommand,2,"r",0,1,1,1,0,0},
	{"hexists",hexistsCommand,3,"rF",0,1,1,1,0,0},
	// {"hscan",hscanCommand,-3,"rR",0,NULL,1,1,1,0,0},
	// {"incrby",incrbyCommand,3,"wmF",0,NULL,1,1,1,0,0},
	// {"decrby",decrbyCommand,3,"wmF",0,NULL,1,1,1,0,0},
	// {"incrbyfloat",incrbyfloatCommand,3,"wmF",0,NULL,1,1,1,0,0},
	{"getset",getsetCommand,3,"wm",0,1,1,1,0,0},
	{"mset",msetCommand,-3,"wm",0,1,-1,2,0,0},
	// {"msetnx",msetnxCommand,-3,"wm",0,NULL,1,-1,2,0,0},
	// {"randomkey",randomkeyCommand,1,"rR",0,NULL,0,0,0,0,0},
	// {"select",selectCommand,2,"rlF",0,NULL,0,0,0,0,0},
	// {"move",moveCommand,3,"wF",0,NULL,1,1,1,0,0},
	// {"rename",renameCommand,3,"w",0,NULL,1,2,1,0,0},
	// {"renamenx",renamenxCommand,3,"wF",0,NULL,1,2,1,0,0},
	// {"expire",expireCommand,3,"wF",0,NULL,1,1,1,0,0},
	// {"expireat",expireatCommand,3,"wF",0,NULL,1,1,1,0,0},
	// {"pexpire",pexpireCommand,3,"wF",0,NULL,1,1,1,0,0},
	// {"pexpireat",pexpireatCommand,3,"wF",0,NULL,1,1,1,0,0},
	// {"keys",keysCommand,2,"rS",0,NULL,0,0,0,0,0},
	// {"scan",scanCommand,-2,"rR",0,NULL,0,0,0,0,0},
	// {"dbsize",dbsizeCommand,1,"rF",0,NULL,0,0,0,0,0},
	// {"auth",authCommand,2,"rsltF",0,NULL,0,0,0,0,0},
	{"ping",pingCommand,-1,"rtF",0,0,0,0,0,0},
	{"echo",echoCommand,2,"rF",0,0,0,0,0,0},
	// {"save",saveCommand,1,"ars",0,NULL,0,0,0,0,0},
	// {"bgsave",bgsaveCommand,1,"ar",0,NULL,0,0,0,0,0},
	// {"bgrewriteaof",bgrewriteaofCommand,1,"ar",0,NULL,0,0,0,0,0},
	// {"shutdown",shutdownCommand,-1,"arlt",0,NULL,0,0,0,0,0},
	// {"lastsave",lastsaveCommand,1,"rRF",0,NULL,0,0,0,0,0},
	// {"type",typeCommand,2,"rF",0,NULL,1,1,1,0,0},
	// {"multi",multiCommand,1,"rsF",0,NULL,0,0,0,0,0},
	// {"exec",execCommand,1,"sM",0,NULL,0,0,0,0,0},
	// {"discard",discardCommand,1,"rsF",0,NULL,0,0,0,0,0},
	// {"sync",syncCommand,1,"ars",0,NULL,0,0,0,0,0},
	// {"psync",syncCommand,3,"ars",0,NULL,0,0,0,0,0},
	// {"replconf",replconfCommand,-1,"arslt",0,NULL,0,0,0,0,0},
	// {"flushdb",flushdbCommand,1,"w",0,NULL,0,0,0,0,0},
	// {"flushall",flushallCommand,1,"w",0,NULL,0,0,0,0,0},
	// {"sort",sortCommand,-2,"wm",0,sortGetKeys,1,1,1,0,0},
	// {"info",infoCommand,-1,"rlt",0,NULL,0,0,0,0,0},
	// {"monitor",monitorCommand,1,"ars",0,NULL,0,0,0,0,0},
	// {"ttl",ttlCommand,2,"rF",0,NULL,1,1,1,0,0},
	// {"pttl",pttlCommand,2,"rF",0,NULL,1,1,1,0,0},
	// {"persist",persistCommand,2,"wF",0,NULL,1,1,1,0,0},
	// {"slaveof",slaveofCommand,3,"ast",0,NULL,0,0,0,0,0},
	// {"role",roleCommand,1,"last",0,NULL,0,0,0,0,0},
	{"debug",debugCommand,-2,"as",0,0,0,0,0,0},
	// {"config",configCommand,-2,"art",0,NULL,0,0,0,0,0},
	// {"subscribe",subscribeCommand,-2,"rpslt",0,NULL,0,0,0,0,0},
	// {"unsubscribe",unsubscribeCommand,-1,"rpslt",0,NULL,0,0,0,0,0},
	// {"psubscribe",psubscribeCommand,-2,"rpslt",0,NULL,0,0,0,0,0},
	// {"punsubscribe",punsubscribeCommand,-1,"rpslt",0,NULL,0,0,0,0,0},
	// {"publish",publishCommand,3,"pltrF",0,NULL,0,0,0,0,0},
	// {"pubsub",pubsubCommand,-2,"pltrR",0,NULL,0,0,0,0,0},
	// {"watch",watchCommand,-2,"rsF",0,NULL,1,-1,1,0,0},
	// {"unwatch",unwatchCommand,1,"rsF",0,NULL,0,0,0,0,0},
	// {"cluster",clusterCommand,-2,"ar",0,NULL,0,0,0,0,0},
	// {"restore",restoreCommand,-4,"awm",0,NULL,1,1,1,0,0},
	// {"restore-asking",restoreCommand,-4,"awmk",0,NULL,1,1,1,0,0},
	// {"migrate",migrateCommand,-6,"aw",0,NULL,0,0,0,0,0},
	// {"asking",askingCommand,1,"r",0,NULL,0,0,0,0,0},
	// {"readonly",readonlyCommand,1,"rF",0,NULL,0,0,0,0,0},
	// {"readwrite",readwriteCommand,1,"rF",0,NULL,0,0,0,0,0},
	// {"dump",dumpCommand,2,"ar",0,NULL,1,1,1,0,0},
	{"object",objectCommand,3,"r",0,2,2,2,0,0},
	// {"client",clientCommand,-2,"ars",0,NULL,0,0,0,0,0},
	// {"eval",evalCommand,-3,"s",0,evalGetKeys,0,0,0,0,0},
	// {"evalsha",evalShaCommand,-3,"s",0,evalGetKeys,0,0,0,0,0},
	// {"slowlog",slowlogCommand,-2,"r",0,NULL,0,0,0,0,0},
	// {"script",scriptCommand,-2,"ras",0,NULL,0,0,0,0,0},
	// {"time",timeCommand,1,"rRF",0,NULL,0,0,0,0,0},
	// {"bitop",bitopCommand,-4,"wm",0,NULL,2,-1,1,0,0},
	// {"bitcount",bitcountCommand,-2,"r",0,NULL,1,1,1,0,0},
	// {"bitpos",bitposCommand,-3,"r",0,NULL,1,1,1,0,0},
	// {"wait",waitCommand,3,"rs",0,NULL,0,0,0,0,0},
	// {"command",commandCommand,0,"rlt",0,NULL,0,0,0,0,0},
	// {"pfselftest",pfselftestCommand,1,"r",0,NULL,0,0,0,0,0},
	// {"pfadd",pfaddCommand,-2,"wmF",0,NULL,1,1,1,0,0},
	// {"pfcount",pfcountCommand,-2,"w",0,NULL,1,1,1,0,0},
	// {"pfmerge",pfmergeCommand,-2,"wm",0,NULL,1,-1,1,0,0},
	// {"pfdebug",pfdebugCommand,-3,"w",0,NULL,0,0,0,0,0},
	// {"latency",latencyCommand,-2,"arslt",0,NULL,0,0,0,0,0}
};

static struct rliteCommand *lookupCommand(const char *name, size_t len) {
	char _name[100];
	if (len >= 100) {
		return NULL;
	}
	memcpy(_name, name, len);
	_name[len] = '\0';
	int j;
	int numcommands = sizeof(rliteCommandTable)/sizeof(struct rliteCommand);

	for (j = 0; j < numcommands; j++) {
		struct rliteCommand *c = rliteCommandTable+j;
		if (strcasecmp(c->name, _name) == 0) {
			return c;
		}
	}

	return NULL;
}