#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

#include "vrt.h"
#include "bin/varnishd/cache.h"

#include "vcc_if.h"

#include <pthread.h>
#include <hiredis/hiredis.h>


#define REDIS_TIMEOUT_MS	200	/* 200 milliseconds */


#define	LOG_E(...) fprintf(stderr, __VA_ARGS__);
#ifdef DEBUG
#	define	LOG_T(...) fprintf(stderr, __VA_ARGS__);
#else
#	define	LOG_T(...) do {} while(0);
#endif

typedef void (*thread_destructor)(void *);

typedef struct redisConfig {
	char *host;
	int port;
	struct timeval timeout;
} config_t;

static pthread_key_t redis_key;
static pthread_once_t redis_key_once = PTHREAD_ONCE_INIT;


static void __match_proto__()
vmod_log(struct sess *sp, const char *fmt, ...)
{
        char buf[8192], *p;
        va_list ap;

        va_start(ap, fmt);
        p = VRT_StringList(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if (p != NULL)
                WSP(sp, SLT_VCL_Log, "%s", buf);
}

static void
free_config(config_t *cfg)
{
  free(cfg->host);
  free(cfg);
}

static void
make_key()
{
	(void)pthread_key_create(&redis_key, (thread_destructor)redisFree);
}

static config_t *
make_config(const char *host, int port, int timeout_ms)
{
	config_t *cfg;

	LOG_T("make_config(%s,%d,%d)\n", host, port, timeout_ms);

	cfg = malloc(sizeof(config_t));
	if(cfg == NULL)
		return NULL;

	if(port <= 0)
		port = 6379;

	if(timeout_ms <= 0)
		timeout_ms = REDIS_TIMEOUT_MS;

	cfg->host = strdup(host);
	cfg->port = port;

	cfg->timeout.tv_sec = timeout_ms / 1000;
	cfg->timeout.tv_usec = (timeout_ms % 1000) * 1000;

	return cfg;
}

int
init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
	config_t *cfg;

	LOG_T("redis init called\n");

	(void)pthread_once(&redis_key_once, make_key);

	if (priv->priv == NULL) {
		priv->priv = make_config("127.0.0.1", 6379, REDIS_TIMEOUT_MS);
		priv->free = (vmod_priv_free_f *)free_config;
	}

	return (0);
}

void
vmod_init_redis(struct sess *sp, struct vmod_priv *priv, const char *host, int port, int timeout_ms)
{
	config_t *old_cfg = priv->priv;

	priv->priv = make_config(host, port, timeout_ms);
	if(priv->priv && old_cfg) {
		free_config(old_cfg);
	}
}

static redisReply *
redis_common(struct sess *sp, struct vmod_priv *priv, const char *command)
{
	config_t *cfg = priv->priv;
	redisContext *c;
	redisReply *reply = NULL;

	LOG_T("redis(%x): running %s %p\n", pthread_self(), command, priv->priv);

	if ((c = pthread_getspecific(redis_key)) == NULL) {
		c = redisConnectWithTimeout(cfg->host, cfg->port, cfg->timeout);
		if (c->err) {
			LOG_E("redis error (connect): %s\n", c->errstr);
		}
		(void)pthread_setspecific(redis_key, c);
	}

	reply = redisCommand(c, command);
	if (reply == NULL && c->err == REDIS_ERR_EOF) {
		c = redisConnectWithTimeout(cfg->host, cfg->port, cfg->timeout);
		if (c->err) {
			LOG_E("redis error (reconnect): %s\n", c->errstr);
			redisFree(c);
		} else {
			redisFree(pthread_getspecific(redis_key));
			(void)pthread_setspecific(redis_key, c);

			reply = redisCommand(c, command);
		}
	}
	if (reply == NULL) {
		LOG_E("redis error (command): err=%d errstr=%s\n", c->err, c->errstr);
	}

	return reply;
}

void
vmod_send(struct sess *sp, struct vmod_priv *priv, const char *command)
{
	redisReply *reply = redis_common(sp, priv, command);
	if (reply != NULL) {
		freeReplyObject(reply);
	}
}

const char *
vmod_call(struct sess *sp, struct vmod_priv *priv, const char *command)
{
	redisReply *reply = NULL;
	const char *ret = NULL;
	char *digits;

	reply = redis_common(sp, priv, command);
	if (reply == NULL) {
		goto done;
	}

	switch (reply->type) {
	case REDIS_REPLY_STATUS:
		ret = WS_Dup(sp->ws, reply->str);
		break;
	case REDIS_REPLY_ERROR:
		ret = WS_Dup(sp->ws, reply->str);
		break;
	case REDIS_REPLY_INTEGER:
		digits = WS_Alloc(sp->ws, 21); /* sizeof(long long) == 8; 20 digits + NUL */
		if(digits)
			sprintf(digits, "%lld", reply->integer);
		ret = digits;
		break;
	case REDIS_REPLY_NIL:
		ret = NULL;
		break;
	case REDIS_REPLY_STRING:
		ret = WS_Dup(sp->ws, reply->str);
		break;
	case REDIS_REPLY_ARRAY:
		ret = WS_Dup(sp->ws, "array");
		break;
	default:
		ret = WS_Dup(sp->ws, "unexpected");
	}

done:
	if (reply) {
		freeReplyObject(reply);
	}

	return ret;
}

void
vmod_pipeline(struct sess *sp, struct vmod_priv *priv)
{
	config_t *cfg = priv->priv;
	redisContext *c;

	LOG_T("redis(%x): pipeline %p\n", pthread_self(), priv->priv);

	if ((c = pthread_getspecific(redis_key)) == NULL) {
		c = redisConnect(cfg->host, cfg->port);
		if (c->err) {
			LOG_E("redis error (connect): %s\n", c->errstr);
		}
		(void)pthread_setspecific(redis_key, c);
	}

	if (c->err == REDIS_ERR_EOF) {
		c = redisConnectWithTimeout(cfg->host, cfg->port, cfg->timeout);
		if (c->err) {
			LOG_E("redis error (reconnect): %s\n", c->errstr);
			redisFree(c);
		} else {
			redisFree(pthread_getspecific(redis_key));
			(void)pthread_setspecific(redis_key, c);
		}
	}
}

void
vmod_push(struct sess *sp, struct vmod_priv *priv, const char *command)
{
	redisContext *c;

	LOG_T("redis(%x): push %s %p\n", pthread_self(), command, priv->priv);

	c = pthread_getspecific(redis_key);
	redisAppendCommand(c, command);
}

const char *
vmod_pop(struct sess *sp, struct vmod_priv *priv)
{
	redisReply *reply;
	const char *ret = NULL;
	char *digits;
	redisContext *c;

	LOG_T("redis(%x): pop %p\n", pthread_self(), priv->priv);

	c = pthread_getspecific(redis_key);
	redisGetReply(c,&reply);

	if (reply == NULL) {
		LOG_E("redis error (command): err=%d errstr=%s\n", c->err, c->errstr);
		goto done;
	}

	switch (reply->type) {
	case REDIS_REPLY_STATUS:
		ret = WS_Dup(sp->ws, reply->str);
		break;
	case REDIS_REPLY_ERROR:
		ret = WS_Dup(sp->ws, reply->str);
		break;
	case REDIS_REPLY_INTEGER:
		digits = WS_Alloc(sp->ws, 21); /* sizeof(long long) == 8; 20 digits + NUL */
		if(digits)
			sprintf(digits, "%lld", reply->integer);
		ret = digits;
		break;
	case REDIS_REPLY_NIL:
		ret = NULL;
		break;
	case REDIS_REPLY_STRING:
		ret = WS_Dup(sp->ws, reply->str);
		break;
	case REDIS_REPLY_ARRAY:
		ret = WS_Dup(sp->ws, "array");
		break;
	default:
		ret = WS_Dup(sp->ws, "unexpected");
	}

done:
	if (reply) {
		freeReplyObject(reply);
	}

	return ret;
}
