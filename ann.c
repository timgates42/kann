#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "kann_rand.h"
#include "kann_min.h"
#include "kann.h"

#define KANN_MAGIC "KAN\1"

int kann_verbose = 3;

kann_t *kann_init(uint64_t seed)
{
	kann_t *a;
	a = (kann_t*)calloc(1, sizeof(kann_t));
	a->rng.data = kann_srand_r(seed);
	a->rng.func = kann_drand;
	return a;
}

void kann_destroy(kann_t *a)
{
	free(a->t); free(a->g);
	if (a->v) kad_free(a->n, a->v);
	free(a->rng.data);
	free(a);
}

kann_min_t *kann_minimizer(const kann_mopt_t *o, int n)
{
	kann_min_t *m;
	m = kann_min_init(KANN_MM_RMSPROP, KANN_MB_CONST, n);
	m->lr = o->lr, m->decay = o->decay;
	return m;
}

void kann_collate_var(kann_t *a)
{
	int i, j, n_par;
	n_par = kann_n_par(a);
	a->t = (float*)realloc(a->t, n_par * sizeof(float));
	a->g = (float*)realloc(a->g, n_par * sizeof(float));
	memset(a->g, 0, n_par * sizeof(float));
	for (i = j = 0; i < a->n; ++i) {
		kad_node_t *v = a->v[i];
		if (kad_is_var(v)) {
			int l;
			l = kad_len(v);
			memcpy(&a->t[j], v->x, l * sizeof(float));
			free(v->x);
			v->x = &a->t[j];
			v->g = &a->g[j];
			j += l;
		}
	}
}

static inline int kann_n_by_label(const kann_t *a, int label)
{
	int i, n = 0;
	for (i = 0; i < a->n; ++i)
		if (a->v[i]->label == label)
			n += a->v[i]->n_d > 1? kad_len(a->v[i]) / a->v[i]->d[0] : 1; // the first dimension is batch size
	return n;
}

int kann_n_in(const kann_t *a) { return kann_n_by_label(a, KANN_L_IN); }
int kann_n_out(const kann_t *a) { return kann_n_by_label(a, KANN_L_OUT); }

void kann_mopt_init(kann_mopt_t *mo)
{
	memset(mo, 0, sizeof(kann_mopt_t));
	mo->lr = 0.01f;
	mo->fv = 0.1f;
	mo->mb_size = 64;
	mo->epoch_lazy = 10;
	mo->max_epoch = 100;
	mo->decay = 0.9f;
}

static void kann_set_batch_size(kann_t *a, int B)
{
	int i;
	for (i = 0; i < a->n; ++i)
		if (a->v[i]->label == KANN_L_IN || a->v[i]->label == KANN_L_TRUTH)
			a->v[i]->d[0] = B;
	for (i = 0; i < a->n; ++i) {
		kad_node_t *p = a->v[i];
		if (p == 0 || p->n_child == 0) continue;
		kad_op_list[p->op](p, KAD_SYNC_DIM);
		kad_op_list[p->op](p, KAD_ALLOC);
		p->x = (float*)realloc(p->x, kad_len(p) * sizeof(float));
		p->g = (float*)realloc(p->g, kad_len(p) * sizeof(float));
	}
}

static int kann_bind_by_label(kann_t *a, int label, float **x)
{
	int i, k;
	for (i = k = 0; i < a->n; ++i)
		if (a->v[i]->n_child == 0 && !a->v[i]->to_back && a->v[i]->label == label)
			a->v[i]->x = x[k++];
	return k;
}

kann_t *kann_rnn_unroll(kann_t *a, int len, int pool_hidden)
{
	kann_t *b;
	b = (kann_t*)calloc(1, sizeof(kann_t));
	b->rng = a->rng, b->t = a->t, b->g = a->g;
	if (pool_hidden) {
		abort();
	} else {
		int i, n_root = 0, k;
		kad_node_t **t, **root;
		b->v = kad_unroll(a->n, a->v, len, &b->n);
		t = (kad_node_t**)calloc(len, sizeof(kad_node_t*));
		root = (kad_node_t**)calloc(len + 1, sizeof(kad_node_t*));
		for (i = k = 0; i < b->n; ++i) {
			if (b->v[i]->label == KANN_L_OUT) root[n_root++] = b->v[i];
			else if (b->v[i]->label == KANN_L_COST) t[k++] = b->v[i], b->v[i]->label = 0;
		}
		assert(k == len && n_root == len);
		root[n_root++] = kad_avg(k, t);
		root[n_root-1]->label = KANN_L_COST;
		free(b->v);
		b->v = kad_compile_array(&b->n, n_root, root);
		free(root); free(t);
	}
	return b;
}

float kann_fnn_mini(kann_t *a, kann_min_t *m, int bs, float **x, float **y) // train or validate a minibatch
{
	int i, i_cost = -1, n_cost = 0;
	float cost;
	for (i = 0; i < a->n; ++i)
		if (a->v[i]->label == KANN_L_COST)
			i_cost = i, ++n_cost;
	assert(n_cost == 1);
	kann_set_batch_size(a, bs);
	kann_bind_by_label(a, KANN_L_IN, x);
	kann_bind_by_label(a, KANN_L_TRUTH, y);
	cost = *kad_eval(a->n, a->v, i_cost);
	if (m) {
		kad_grad(a->n, a->v, i_cost);
		kann_min_mini_update(m, a->g, a->t);
	}
	return cost;
}

void kann_train_fnn(const kann_mopt_t *mo, kann_t *a, int n, float **_x, float **_y)
{
	float **x, **y, *bx, *by;
	int i, n_train, n_validate, n_in, n_out, n_par;
	kann_min_t *min;

	// copy and shuffle
	x = (float**)malloc(n * sizeof(float*));
	y = _y? (float**)malloc(n * sizeof(float*)) : 0;
	for (i = 0; i < n; ++i) {
		x[i] = _x[i];
		if (y) y[i] = _y[i];
	}
	kann_shuffle(a->rng.data, n, x, y, 0);

	// set validation set
	n_validate = mo->fv > 0.0f && mo->fv < 1.0f? (int)(mo->fv * n + .499) : 0;
	n_train = n - n_validate;

	// prepare mini-batch buffer
	n_in = kann_n_in(a);
	n_out = kann_n_out(a);
	n_par = kann_n_par(a);
	bx = (float*)malloc(mo->mb_size * n_in * sizeof(float));
	by = (float*)malloc(mo->mb_size * n_out * sizeof(float));
	min = kann_minimizer(mo, n_par);

	// main loop
	for (i = 0; i < mo->max_epoch; ++i) {
		int n_proc = 0;
		double running_cost = 0.0, val_cost = 0.0;
		kann_shuffle(a->rng.data, n_train, x, y, 0);
		while (n_proc < n_train) {
			int j, mb = n_train - n_proc < mo->mb_size? n_train - n_proc : mo->mb_size;
			for (j = 0; j < mb; ++j) {
				memcpy(&bx[j*n_in],  x[n_proc+j], n_in  * sizeof(float));
				memcpy(&by[j*n_out], y[n_proc+j], n_out * sizeof(float));
			}
			running_cost += kann_fnn_mini(a, min, mb, &bx, &by) * mb;
			n_proc += mb;
		}
		n_proc = 0;
		while (n_proc < n_validate) {
			int j, mb = n_validate - n_proc < mo->mb_size? n_validate - n_proc : mo->mb_size;
			for (j = 0; j < mb; ++j) {
				memcpy(&bx[j*n_in],  x[n_proc+j], n_in  * sizeof(float));
				memcpy(&by[j*n_out], y[n_proc+j], n_out * sizeof(float));
			}
			val_cost += kann_fnn_mini(a, 0, mb, &bx, &by) * mb;
			n_proc += mb;
		}
		if (kann_verbose >= 3) {
			if (n_validate == 0) fprintf(stderr, "running cost: %g\n", running_cost / n_train);
			else fprintf(stderr, "running cost: %g; validation cost: %g\n", running_cost / n_train, val_cost / n_validate);
		}
	}

	// free
	kann_min_destroy(min);
	free(by); free(bx);
	free(y); free(x);
}

const float *kann_apply_fnn1(kann_t *a, float *x) // FIXME: for now it doesn't work RNN
{
	int i;
	kann_set_batch_size(a, 1);
	kann_bind_by_label(a, KANN_L_IN, &x);
	kad_eval_by_label(a->n, a->v, KANN_L_OUT);
	for (i = 0; i < a->n; ++i)
		if (a->v[i]->label == KANN_L_OUT)
			return a->v[i]->x;
	return 0;
}

/*************
 * Model I/O *
 *************/

void kann_write(const char *fn, const kann_t *ann)
{
	FILE *fp;
	fp = fn && strcmp(fn, "-")? fopen(fn, "wb") : stdout;
	fwrite(KANN_MAGIC, 1, 4, fp);
	kad_write(fp, ann->n, ann->v);
	fwrite(ann->t, sizeof(float), kann_n_par(ann), fp);
	fclose(fp);
}

kann_t *kann_read(const char *fn)
{
	FILE *fp;
	char magic[4];
	kann_t *ann;
	int i, j, n_par;

	fp = fn && strcmp(fn, "-")? fopen(fn, "rb") : stdin;
	fread(magic, 1, 4, fp);
	if (strncmp(magic, KANN_MAGIC, 4) != 0) {
		fclose(fp);
		return 0;
	}
	ann = (kann_t*)calloc(1, sizeof(kann_t));
	ann->rng.data = kann_srand_r(11);
	ann->rng.func = kann_drand;
	ann->v = kad_read(fp, &ann->n);
	n_par = kann_n_par(ann);
	ann->t = (float*)malloc(n_par * sizeof(float));
	ann->g = (float*)calloc(n_par, sizeof(float));
	fread(ann->t, sizeof(float), n_par, fp);
	fclose(fp);

	for (i = j = 0; i < ann->n; ++i) {
		kad_node_t *p = ann->v[i];
		if (p->n_child == 0 && p->to_back) {
			p->x = &ann->t[j];
			p->g = &ann->g[j];
			j += kad_len(p);
		}
	}
	assert(j == n_par);
	return ann;
}
