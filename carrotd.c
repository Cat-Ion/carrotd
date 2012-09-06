#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "marcov/marcov.h"

#define EVERRRRRRRR ;;

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
	marcov_t *marcov;
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
	marcov_t *m = *(marcov_t **)node;

	int cmp = strncmp(s->prefix, m->key, strlen(s->prefix));
	
	if(cmp < 0) {
		return 0;
	} else if(cmp == 0) {
		int i;
		for(i = s->n - 1;
		    i >= 0 && (s->d[i].key == NULL || m->total > s->d[i].n);
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
	int prefixlen = d->marcov->order;
	marcov_t *n;
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

	if(prefixlen > w->num - 1) {
		prefixlen = w->num - 1;
	}
	
	do {
		w->num--;
		n = marcov_find_prefix(d->marcov, w, prefixlen--);
		w->num++;
		if(!n) {
			continue;
		}

		walkdata.d = calloc(num + 1, sizeof(struct{char *key; int n;}));
		
		marcov_walk(n, twalk_predict, &walkdata);

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
	
	r->num = 0;
	if(walkdata.d) {
		for(int i = 0; i < num && walkdata.d[i].n > 0; i++) {
			r->num++;
			r->w[i] = walkdata.d[i].key;
		}
	}
	return r;
}
	
int dict_add(dict_t *d, wordlist_t *w) {
	int ret = -1;
	for(int i = 0; i < w->num; i++) {
		w->w[i] = stringidx(&(d->strings), w->w[i]);
	}

	pthread_mutex_lock(&(d->write));
	while(d->readers > 0) {
		pthread_cond_wait(&(d->noreaders), &(d->write));
	}

	if(w->num >= d->marcov->order) {
		int n = w->num;
		for(; w->num >= d->marcov->order; w->num--) {
			marcov_add(d->marcov, w);
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
		.num = d->marcov->order + 1,
		.w = malloc((d->marcov->order + 1) * sizeof(char*))
	};

	for(int i = 0; i < w->num - d->marcov->order; i++) {
		for(int j = 0; j < x.num; j++)
			x.w[j] = w->w[i+j];
		marcov_dec(d->marcov, &x);
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

	(*d)[*numdicts - 1].marcov = NULL;
	int fd = open(path, O_RDONLY);
	if(fd == -1) {
		*numdicts -= 1;
		return -1;
	}
	(*d)[*numdicts - 1].strings = NULL;
	marcov_load(&((*d)[*numdicts - 1].strings), &((*d)[*numdicts - 1].marcov), fd);
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
		marcov_dump(d->strings, d->marcov, fd);
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

	if(sfd == -1) {
		perror("socket");
		return -1;
	}

	memset(&sa, 0, sizeof(sa));

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = INADDR_ANY;

	if(bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
		perror("bind");
		return -1;
	}

	if(listen(sfd, 32) == -1) {
		perror("listen");
		return -1;
	}

	return sfd;
}

int main(int argc, char **argv) {
	struct status_s status;
	status.fd = listen_port(3245);
	status.numdicts = 0;
	status.dicts = NULL;

	if(status.fd < 0) {
		exit(EXIT_FAILURE);
	}

	fd_set rfds;
	int maxfd = status.fd;

	FD_SET(status.fd, &rfds);

	for(EVERRRRRRRR)  {
		fd_set tmp = rfds;
		select(maxfd + 1, &tmp, NULL, NULL, NULL);
		if(FD_ISSET(status.fd, &tmp)) {
			int cfd = accept(status.fd, NULL, NULL);
			FD_SET(cfd, &rfds);
			if(cfd > maxfd) {
				maxfd = cfd;
			}
		}
		for(int fd = maxfd; fd > status.fd; fd--) {
			if(!FD_ISSET(fd, &tmp)) {
				continue;
			}
			
			message_t msg;
			int result;
			wordlist_t msgwords, *reply = NULL;
			char *txtdata;
			int txti = 0;

			if(read(fd, &msg, sizeof(msg)) == 0) {
				FD_CLR(fd, &rfds);
				if(fd == maxfd) {
					maxfd--;
				}
				continue;
			}

			txtdata = malloc(msg.length - sizeof(msg) + sizeof(msg.length));
			read(fd, txtdata, msg.length - sizeof(msg) + sizeof(msg.length));
			
			msgwords.num = msg.data;
			msgwords.w = malloc(msgwords.num * sizeof(char *));
			for(int i = 0; i < msgwords.num; i++) {
				msgwords.w[i] = (word_t *)(txtdata + txti)->data;
				txti += (word_t *)(txtdata + txti)->length + 1;
			}
			
			if((msg.type == MESSAGE_TYPE_REQUEST ||
			    msg.type == MESSAGE_TYPE_DEL ||
			    msg.type == MESSAGE_TYPE_ADD ||
			    msg.type == MESSAGE_TYPE_SAVE) &&
			   (msg.dict < 0 || msg.dict >= status.numdicts)) {
				result = -1;
			} else switch(msg.type) {
				case MESSAGE_TYPE_REQUEST:
					reply = dict_predict(status.dicts + msg.dict, &msgwords, msg.data);
					result = reply == NULL ? -1 : reply->num;
					break;
				case MESSAGE_TYPE_ADD:
					result = dict_add(status.dicts + msg.dict, &msgwords);
					break;
				case MESSAGE_TYPE_DEL:
					result = dict_del(status.dicts + msg.dict, &msgwords);
					break;
				case MESSAGE_TYPE_LOAD:
					result = dict_load(&(status.numdicts), &(status.dicts), msgwords.w[0]);
					break;
				case MESSAGE_TYPE_SAVE:
					result = dict_save(status.dicts + msg.dict, msg.data > 0 ? msgwords.w[0] : status.dicts[msg.dict].path);
					break;
				}
			
			message_t answer = (message_t) {
				.length = sizeof(message_t) - 4,
				.id = msg.id,
				.type = result >= 0 ? msg.type : -1,
				.dict = msg.dict,
				.data = 0
			};
			
			switch(msg.type) {
			case MESSAGE_TYPE_LOAD:
				answer.dict = result;
				answer.data = status.dicts[result].marcov->order;
				break;
			case MESSAGE_TYPE_REQUEST:
				answer.data = result;
				
				answer.length += (sizeof(int32_t) + 1) * result;
				for(int i = 0; i < result; i++) {
					answer.length += strlen(reply->w[i]);
				}
				break;
			}
			
			write(fd, &answer, sizeof(message_t));
			
			if(msg.type == MESSAGE_TYPE_REQUEST) {
				for(int i = 0; i < result; i++) {
					int32_t len = strlen(reply->w[i]);
					write(fd, &len, sizeof(int32_t));
					write(fd, reply->w[i], len + 1);
				}
				free(reply->w);
				free(reply);
			}
			
			free(txtdata);
		}
	}
	
	return 0;
}
