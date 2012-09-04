#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include "marcov/markov.h"

#define EVERRRRRRRR (;;)

enum {
	MESSAGE_TYPE_REQUEST = 0,
	MESSAGE_TYPE_ADD     = 1,
	MESSAGE_TYPE_DEL     = 2,
	MESSAGE_TYPE_LOAD    = 3,
	MESSAGE_TYPE_SAVE    = 4,
	MESSAGE_TYPE_NUM     = 5
};

typedef struct {
	struct timespec lastwrite;
	
	int readers;
	pthread_cond_t noreaders;
	pthread_mutex_t write;

	char *path;
	markov_t *markov;
	void *strings;
} dict_t;

typedef struct {
	int32_t length; /* Length of the char[] data */
	char data[];
} word_t;

typedef struct {
	int32_t length; /* Length of the complete message, sans the length field itself */
	int32_t id;     /* Unique number that gets echoed in the server's reply */
	int32_t type;   /* One of MESSAGE_TYPE_*, as above */
	int32_t dict;   /* The dictionary's identifier. Returned by the
	                   answer to MESSAGE_TYPE_LOAD, valid for all
	                   commands except for LOAD */
	int32_t data;   /* Number of words following the message for
	                   ADD/CHANGE/DEL, or number of expected
	                   completions for REQUEST */
} message_t;

struct status_s {
	int numdicts;
	dict_t *dicts;
	int fd;
};

int twalk_predict(const void *node, VISIT v, int depth, void *data) {
	struct {
		char *prefix;
		int n;
		struct {
			char *key;
			int n;
		} *d;
	} *s = data;
	markov_t *m = *(markov_t **)node;

	int cmp = strncmp(s->prefix, m->key, strlen(s->prefix));
	
	if(cmp > 0) {
		return 0;
	} else if(cmp == 0) {
		int i;
		for(i = s->n - 1;
		    i >= 0 && (m->total > s->d[i].n || !(s->d[i].key));
		    i--) {
			s->d[i+1] = s->d[i];
		}
		i++;
		if(i < s->n) {
			s->d[i].n = m->total;
			s->d[i].key = m->key;
		}
		return 1;
	} else {
		return 1;
	}
}

wordlist_t *dict_predict(dict_t *d, wordlist_t *w, int num) {
	int prefixlen = d->markov->order;
	markov_t *n;
	wordlist_t *r = malloc(sizeof(wordlist_t));
	r->num = num;
	r->w = calloc(num, sizeof(char *));
	struct {
		char *prefix;
		int n;
		struct {
			char *key;
			int n;
		} *d;
	} walkdata;

	walkdata.prefix = w->w[w->num - 1];
	walkdata.n = num;
	walkdata.d = NULL;

	pthread_mutex_lock(&(d->write));
	d->readers++;
	pthread_mutex_unlock(&(d->write));
	
	do {
		w->num--;
		n = markov_find_prefix(d->markov, w, prefixlen--);
		w->num++;
		if(!n)
			continue;

		walkdata.d = calloc(num + 1, sizeof(struct{char *key; int n;}));
		
		markov_walk(n, twalk_predict, &walkdata);

		if(walkdata.d[0].n == 0) {
			free(walkdata.d);
			walkdata.d = NULL;
		}
	} while(prefixlen >= 0 && walkdata.d == NULL);

	pthread_mutex_lock(&(d->write));
	d->readers--;
	if(d->readers == 0) {
		pthread_cond_signal(&(d->noreaders));
	}
	pthread_mutex_unlock(&(d->write));
	
	if(prefixlen < 0)
		return NULL;

	r->num = 0;
	for(int i = 0; i < num && walkdata.d[i].n > 0; i++) {
		r->w[i] = walkdata.d[i].key;
	}
	return r;
}
	
int dict_add(dict_t *d, wordlist_t *w) {
	int ret = -1;
	pthread_mutex_lock(&(d->write));
	while(d->readers > 0) {
		pthread_cond_wait(&(d->noreaders), &(d->write));
	}

	if(w->num >= d->markov->order) {
		int n = w->num;
		for(; w->num >= d->markov->order; w->num--) {
			markov_add(d->markov, w);
		}
		w->num = ret = n;
	}

	pthread_cond_signal(&(d->noreaders));
	pthread_mutex_unlock(&(d->write));
	return ret;
}
int dict_del(dict_t *d, wordlist_t *w) {
	int ret = -1;
	pthread_mutex_lock(&(d->write));
	while(d->readers > 0) {
		pthread_cond_wait(&(d->noreaders), &(d->write));
	}
	
	wordlist_t x = (wordlist_t) {
		.num = d->markov->order + 1,
		.w = malloc((d->markov->order + 1) * sizeof(char*))
	};

	for(int i = 0; i < w->num - d->markov->order; i++) {
		for(int j = 0; j < x.num; j++)
			x.w[j] = w->w[i+j];
		markov_dec(d->markov, &x);
	}

	pthread_cond_signal(&(d->noreaders));
	pthread_mutex_unlock(&(d->write));
	return ret;
}
int dict_load(int *numdicts, dict_t **d, char *path) {
	for(int i = 0; i < *numdicts; i++) {
		if(!strcmp((*d)[i].path, path)) {
			return i;
		}
	}
	*numdicts += 1;
	
	dict_t *tmp = realloc(*d, *numdicts * sizeof(dict_t));
	if(!tmp) {
		return -1;
	}

	*d = tmp;

	clock_gettime(CLOCK_REALTIME, &((*d)[*numdicts-1].lastwrite));
	(*d)[*numdicts - 1].readers = 0;
	pthread_cond_init(&((*d)[*numdicts - 1].noreaders), NULL);
	pthread_mutex_init(&((*d)[*numdicts - 1].write), NULL);
	(*d)[*numdicts - 1].path = strdup(path);

	(*d)[*numdicts - 1].markov = NULL;
	int fd = open(path, O_RDONLY);
	if(fd == -1) {
		*numdicts -= 1;
		return -1;
	}
	(*d)[*numdicts - 1].strings = NULL;
	markov_load(&((*d)[*numdicts - 1].strings), &((*d)[*numdicts - 1].markov), fd);
	close(fd);
	return *numdicts - 1;
}

int dict_save(dict_t *d, char *path) {
	int ret = -1;
	pthread_mutex_lock(&(d->write));
	d->readers++;
	pthread_mutex_unlock(&(d->write));

	int fd = open(path, O_WRONLY);
	if(fd < 0) {
		ret = -1;
	} else {
		markov_dump(d->strings, d->markov, fd);
		close(fd);
	}
	
	pthread_mutex_lock(&(d->write));
	d->readers--;
	if(d->readers == 0) {
		pthread_cond_signal(&(d->noreaders));
	}
	pthread_mutex_unlock(&(d->write));
	return ret;
}

int listen_port(int port) {
	struct sockaddr_in sa;
	int sfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

	if(sfd == -1)
		return -1;

	memset(&sa, 0, sizeof(sa));

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = INADDR_ANY;

	if(bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) == -1)
		return -1;

	if(listen(sfd, 32) == -1)
		return -1;

	return sfd;
}

int main(int argc, char **argv) {
	struct status_s status;
	status.fd = listen_port(3245);
	status.numdicts = 0;
	status.dicts = NULL;

	for EVERRRRRRRR  {
		int cfd = accept(status.fd, NULL, NULL);
		message_t msg;
		if(cfd < 0)
			break;

		read(cfd, &msg, sizeof(msg));
		/* TODO: Read words, handle message */
	}

	return 0;
}
