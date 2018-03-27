
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    u_char                       color;
    u_char                       dummy;
    u_short                      len;    //key长度
    ngx_queue_t                  queue;  //队列
    ngx_msec_t                   last;   //更新时间
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   excess; //出来中请求数
    /* ngx_uint_t                   count; */
    u_char                       data[1];//key名称
} ngx_http_limit_proxy_req_node_t;


typedef struct {
    ngx_rbtree_t                  rbtree;  //红黑树
    ngx_rbtree_node_t             sentinel;//空节点
    ngx_queue_t                   queue;   //维护一个全量数据队列，用于清理老数据
} ngx_http_limit_proxy_req_shctx_t;

typedef struct {
    ngx_str_t                     lim_key;  //限流关键字
    ngx_uint_t                    rate;     //平均流量
} ngx_http_limit_proxy_req_prate_t;

typedef struct {
	ngx_str_t		var; //变量名
	ngx_int_t		index; //变量下标
} ngx_http_limit_proxy_req_variable_t;	

typedef struct {
    ngx_http_limit_proxy_req_shctx_t  *sh;
    ngx_slab_pool_t             *shpool;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   rate;      //默认限制流量
    //ngx_int_t                    index;     //变量下标
    //ngx_str_t                    var;       //变量名
    ngx_array_t 		*limit_vars;
    ngx_http_limit_proxy_req_node_t   *node;
} ngx_http_limit_proxy_req_ctx_t;


typedef struct {
    ngx_shm_zone_t              *shm_zone;   //共享内存
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   burst;      //峰值
    ngx_http_limit_proxy_req_prate_t   prate;//key+rate
    ngx_uint_t                   nodelay; /* unsigned  nodelay:1 */
} ngx_http_limit_proxy_req_limit_t;


typedef struct {
    ngx_array_t                  limits;             //limit_proxy_req 数组
    ngx_uint_t                   limit_log_level;    //日志等级
    ngx_uint_t                   delay_log_level;    
    ngx_uint_t                   status_code;        //返回状态码
} ngx_http_limit_proxy_req_conf_t;


//在红黑树中查找该次请求是否超流量限制
static ngx_int_t ngx_http_limit_proxy_req_lookup(ngx_http_limit_proxy_req_limit_t *limit,
		uint32_t hash, ngx_uint_t *ep,ngx_http_request_t *r,ngx_int_t len);

//更新改次请求计数和更新时间
//static ngx_msec_t ngx_http_limit_proxy_req_account(ngx_http_limit_proxy_req_limit_t *limits,
//		ngx_uint_t n, ngx_http_limit_proxy_req_limit_t **limit);

//删除长时间没有请求的节点，默认1分钟无请求则删除
static void ngx_http_limit_proxy_req_expire(ngx_http_limit_proxy_req_ctx_t *ctx,
    ngx_uint_t n, ngx_http_limit_proxy_req_limit_t *limit);

static void *ngx_http_limit_proxy_req_create_conf(ngx_conf_t *cf);
static char *ngx_http_limit_proxy_req_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_limit_proxy_req_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_limit_proxy_req(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_limit_proxy_req_init(ngx_conf_t *cf);

static ngx_int_t ngx_http_limit_proxy_req_var_compare(ngx_http_request_t *r, ngx_http_limit_proxy_req_ctx_t *ctx, u_char *data, u_char *last);

static ngx_int_t ngx_http_limit_req_copy_variables(ngx_http_request_t *r, uint32_t *hash, ngx_http_limit_proxy_req_ctx_t *ctx, ngx_http_limit_proxy_req_node_t *node);

static ngx_conf_enum_t  ngx_http_limit_proxy_req_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_conf_num_bounds_t  ngx_http_limit_proxy_req_status_bounds = {
    ngx_conf_check_num_bounds, 400, 599
};


static ngx_command_t  ngx_http_limit_proxy_req_commands[] = {

    { ngx_string("limit_proxy_req_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_2MORE,
      ngx_http_limit_proxy_req_zone,
      0,
      0,
      NULL },

    { ngx_string("limit_proxy_req"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_2MORE,
      ngx_http_limit_proxy_req,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_proxy_req_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_proxy_req_conf_t, limit_log_level),
      &ngx_http_limit_proxy_req_log_levels },

    { ngx_string("limit_proxy_req_status"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_proxy_req_conf_t, status_code),
      &ngx_http_limit_proxy_req_status_bounds },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_limit_proxy_req_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_limit_proxy_req_init,               /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_limit_proxy_req_create_conf,        /* create location configuration */
    ngx_http_limit_proxy_req_merge_conf          /* merge location configuration */
};


ngx_module_t  ngx_http_limit_proxy_req_module = {
    NGX_MODULE_V1,
    &ngx_http_limit_proxy_req_module_ctx,        /* module context */
    ngx_http_limit_proxy_req_commands,           /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_limit_proxy_req_handler(ngx_http_request_t *r)
{
    //size_t                       len;
    uint32_t                     hash;
    ngx_int_t                    rc, total_len, result;
    ngx_uint_t                   n, excess;
    //ngx_http_variable_value_t   *vv;
    ngx_http_limit_proxy_req_ctx_t    *ctx;
    ngx_http_limit_proxy_req_conf_t   *lrcf;
    ngx_http_limit_proxy_req_limit_t  *limit, *limits;
    u_char                        *data, *last;

    if (r->main->limit_req_set) {
        return NGX_DECLINED;
    }

    lrcf = ngx_http_get_module_loc_conf(r, ngx_http_limit_proxy_req_module);
    limits = lrcf->limits.elts;

    excess = 0;

    rc = NGX_DECLINED;

#if (NGX_SUPPRESS_WARN)
    limit = NULL;
#endif
    reslt = 1;
    for (n = 0; n < lrcf->limits.nelts; n++) {

        limit = &limits[n];
        ctx = limit->shm_zone->data;
	// find lim_key match req variable
	data = limit->prate.lim_key.data;
        last = limit->prate.lim_key.data + limit->prate.lim_key.len;

        //检查是否是默认lim_key, 且只有最后一个有效
        if(n+1 == lrcf->limits.nelts){
          result = ngx_strcmp(data, "*");
        }

        if(result){
          result = ngx_http_limit_proxy_req_var_compare(r, ctx, data, last);

          if (result){
            continue;
          }
        }
        // find or add node in rbtree
        ngx_crc32_init(hash);

        total_len = ngx_http_limit_req_copy_variables(r, &hash, ctx, NULL);
        if (total_len == 0) {
            continue;
        }

        ngx_crc32_final(hash);

	ngx_shmtx_lock(&ctx->shpool->mutex);

        rc = ngx_http_limit_proxy_req_lookup(limit, hash, &excess, r, total_len);

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        if (rc != NGX_AGAIN) {
            break;
        }
    }
   

    r->main->limit_req_set = 1;

    if (rc == NGX_BUSY || rc == NGX_ERROR) {
	ctx = limit->shm_zone->data;
        
	if (rc == NGX_BUSY) {
	   	ngx_log_error(lrcf->limit_log_level, r->connection->log, 0,
                          "limiting requests, excess: %ui.%03ui by zone \"%V\",limit_key:\"%V\" limit rate:%d",
                          excess / 1000, excess % 1000, &limit->shm_zone->shm.name, &limit->prate.lim_key,
			  limit->prate.rate?(limit->prate.rate / 1000):(ctx->rate / 1000));
        }

	ctx->node = NULL;

	return lrcf->status_code;
    }

/*
    if(n >= lrcf->limits.nelts)
    {
	    n = lrcf->limits.nelts - 1 ;
    }

    ngx_http_limit_proxy_req_account(limits, n, &limit);
*/  
    return NGX_DECLINED;

}

static void
ngx_http_limit_proxy_req_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t          **p;
    ngx_http_limit_proxy_req_node_t   *lrn, *lrnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            lrn = (ngx_http_limit_proxy_req_node_t *) &node->color;
            lrnt = (ngx_http_limit_proxy_req_node_t *) &temp->color;

            p = (ngx_memn2cmp(lrn->data, lrnt->data, lrn->len, lrnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_int_t
ngx_http_limit_proxy_req_lookup(ngx_http_limit_proxy_req_limit_t *limit, uint32_t hash, ngx_uint_t *ep, ngx_http_request_t *r, ngx_int_t len)
{
    size_t                      size;
    ngx_int_t                   rc,excess;
    ngx_time_t                 *tp;
    ngx_msec_t                  now;
    u_char                      *lr_data, *lr_last;
    ngx_msec_int_t              ms;
    ngx_rbtree_node_t          *node, *sentinel;
    ngx_http_limit_proxy_req_ctx_t   *ctx;
    ngx_http_limit_proxy_req_node_t  *lr;


    tp = ngx_timeofday();
    now = (ngx_msec_t) (tp->sec * 1000 + tp->msec);

    ctx = limit->shm_zone->data;

    node = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;

    rc = -1;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        lr = (ngx_http_limit_proxy_req_node_t *) &node->color;

        lr_data = lr->data;
        lr_last = lr_data + lr->len;
        
	rc = ngx_http_limit_proxy_req_var_compare(r, ctx, lr_data, lr_last);

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "limit_req lookup is : %i, size is %ui",
                       rc, ctx->limit_vars->nelts);

        if (rc == 0) {
            ngx_queue_remove(&lr->queue);
            ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);

            ms = (ngx_msec_int_t) (now - lr->last);
	    
	    
	    ngx_uint_t rate = ctx->rate;

	    if (limit->prate.rate > 0)
	    {
		    rate = limit->prate.rate;
	    }

            excess = lr->excess - rate * ngx_abs(ms) / 1000 + 1000;
            
	    if (excess < 0) {
                excess = 0;
            }
	
	    *ep = excess;
            if ( (ngx_uint_t)excess > limit->burst) {
		    
		if(lr->excess < limit->burst)
		{
		    lr->last = now;
                    lr->excess = limit->burst;
		}
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "lookup: now:%d, last:%d, excess:%d, lr excess:%d, burst:%d", now,lr->last, excess,lr->excess,limit->burst);
		return NGX_BUSY;
            }
            
	    lr->excess = excess;
	    lr->last = now;
            ctx->node = lr;
            return NGX_OK;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    *ep = 0;

    size = offsetof(ngx_rbtree_node_t, color)
           + offsetof(ngx_http_limit_proxy_req_node_t, data)
           + len;

    ngx_http_limit_proxy_req_expire(ctx, 1, limit);

    node = ngx_slab_alloc_locked(ctx->shpool, size);

    if (node == NULL) {
        ngx_http_limit_proxy_req_expire(ctx, 0, limit);

        node = ngx_slab_alloc_locked(ctx->shpool, size);
        if (node == NULL) {
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                          "could not allocate node%s", ctx->shpool->log_ctx);
            return NGX_ERROR;
        }
    }

    lr = (ngx_http_limit_proxy_req_node_t *) &node->color;
    node->key = hash;

    lr = (ngx_http_limit_proxy_req_node_t *) &node->color;

    lr->len = (u_char) len;
    lr->excess = 0;

    ngx_http_limit_req_copy_variables(r, &hash, ctx, lr);
    ngx_rbtree_insert(&ctx->sh->rbtree, node);
    ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);

    lr->last = now;
    ctx->node = lr;

    return NGX_OK;
}

static ngx_int_t
ngx_http_limit_proxy_req_var_compare(ngx_http_request_t *r, ngx_http_limit_proxy_req_ctx_t *ctx, u_char *data, u_char *last)
{

	size_t                          lr_vv_len;  
	ngx_http_variable_value_t       *vv; 
	ngx_int_t 			rc;
        ngx_uint_t	i;
	ngx_http_limit_proxy_req_variable_t   *lrv; 

	rc = -1;
	lrv =  ctx->limit_vars->elts;
	//判断节点是否相同
        for (i = 0; i < ctx->limit_vars->nelts; i++) {
            vv = ngx_http_get_indexed_variable(r, lrv[i].index);

            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "limit_req vv is %i %v node is %s",
                           lrv[i].index, vv, data);

            lr_vv_len = ngx_min(last - data, vv->len);

            if ((rc = ngx_memcmp(vv->data, data, lr_vv_len)) != 0) {
                break;
            }

            if (lr_vv_len != vv->len) {
                rc = 1;
                break;
            }

            /* lr_vv_len == vv->len */
            data += lr_vv_len;
        }

	if (rc == 0 && last > data){
		rc = -1;
	}
		
	return rc;

}

/*
static ngx_msec_t
ngx_http_limit_proxy_req_account(ngx_http_limit_proxy_req_limit_t *limits, ngx_uint_t n, ngx_http_limit_proxy_req_limit_t **limit)
{
    ngx_int_t                   excess;
    ngx_time_t                 *tp;
    ngx_msec_t                  now;
    ngx_msec_int_t              ms;
    ngx_http_limit_proxy_req_ctx_t   *ctx;
    ngx_http_limit_proxy_req_node_t  *lr;
    ngx_int_t                   rate;

    ctx = limits[n].shm_zone->data;
    lr = ctx->node;

    if (lr == NULL) {
	    return 0;
    }

    ngx_shmtx_lock(&ctx->shpool->mutex);

    tp = ngx_timeofday();

    now = (ngx_msec_t) (tp->sec * 1000 + tp->msec);
    ms = (ngx_msec_int_t) (now - lr->last);

    rate = ctx->rate;

    if(limits[n].prate.rate > 0)
    {
	    rate = limits[n].prate.rate;
    }

    excess = lr->excess - rate * ngx_abs(ms) / 1000 + 1000;

    if (excess < 0) {
	    excess = 0;
    }

    lr->last = now;
    lr->excess = excess;
//    lr->count--;

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    ctx->node = NULL;
    return 0;
}
*/
static ngx_int_t
ngx_http_limit_req_copy_variables(ngx_http_request_t *r, uint32_t *hash,
    ngx_http_limit_proxy_req_ctx_t *ctx, ngx_http_limit_proxy_req_node_t *node)
{
    u_char                        *p;
    size_t                         len, total_len;
    ngx_uint_t                     j;
    ngx_http_variable_value_t     *vv;
    ngx_http_limit_proxy_req_variable_t *lrv;

    total_len = 0;
    p = NULL;

    if (node != NULL) {
        p = node->data;
    }

    lrv = ctx->limit_vars->elts;
    for (j = 0; j < ctx->limit_vars->nelts; j++) {
        vv = ngx_http_get_indexed_variable(r, lrv[j].index);
        if (vv == NULL || vv->not_found) {
            total_len = 0;
            break;
        }

        len = vv->len;

        if (len == 0) {
            total_len = 0;
            break;
        }

        if (len > 65535) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "the value of the \"%V\" variable "
                          "is more than 65535 bytes: \"%v\"",
                          &lrv[j].var, vv);
            total_len = 0;
            break;
        }

        if (node == NULL) {
            total_len += len;
            ngx_crc32_update(hash, vv->data, len);
        } else {
            p = ngx_cpymem(p, vv->data, len);
        }
    }

    return total_len;
}

static void
ngx_http_limit_proxy_req_expire(ngx_http_limit_proxy_req_ctx_t *ctx, ngx_uint_t n, ngx_http_limit_proxy_req_limit_t *limit)
{
	ngx_int_t                   excess;
	ngx_time_t                 *tp;
	ngx_msec_t                  now;
	ngx_queue_t                *q;
	ngx_msec_int_t              ms;
	ngx_rbtree_node_t          *node;
	ngx_http_limit_proxy_req_node_t  *lr;
	ngx_int_t                  rate;
	tp = ngx_timeofday();

	now = (ngx_msec_t) (tp->sec * 1000 + tp->msec);

	/*
	 * n == 1 deletes one or two zero rate entries
	 * n == 0 deletes oldest entry by force
     *        and one or two zero rate entries
     */

    rate = ctx->rate;
    if(limit->prate.rate > 0)
    {
         rate = limit->prate.rate;
    }

    while (n < 3) {

        if (ngx_queue_empty(&ctx->sh->queue)) {
            return;
        }

        q = ngx_queue_last(&ctx->sh->queue);

        lr = ngx_queue_data(q, ngx_http_limit_proxy_req_node_t, queue);
        
	if (n++ != 0) {

            ms = (ngx_msec_int_t) (now - lr->last);
            ms = ngx_abs(ms);

            if (ms < 60000) {
                return;
            }

            excess = lr->excess - rate * ms / 1000;
            //excess = lr->excess - ctx->rate * ms / 1000;

            if (excess > 0) {
                return;
            }
        }

        ngx_queue_remove(q);

        node = (ngx_rbtree_node_t *)
                   ((u_char *) lr - offsetof(ngx_rbtree_node_t, color));

        ngx_rbtree_delete(&ctx->sh->rbtree, node);

        ngx_slab_free_locked(ctx->shpool, node);
    }
}


static ngx_int_t
ngx_http_limit_proxy_req_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_limit_proxy_req_ctx_t  *octx = data;

    size_t                     len;
    ngx_http_limit_proxy_req_ctx_t  *ctx;
    ngx_http_limit_proxy_req_variable_t *v1, *v2;
    ngx_uint_t                   i,j;

    ctx = shm_zone->data;
    v1 = ctx->limit_vars->elts;
    if (octx) {
	    v2 = octx->limit_vars->elts;
	    if (ctx->limit_vars->nelts != octx->limit_vars->nelts) {
		    ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
				    "limit_req \"%V\" uses the \"%V\" variable "
				    "while previously it used the \"%V\" variable",
				    &shm_zone->shm.name, &v1[0].var, &v2[0].var);
		    return NGX_ERROR;
	    }    

	    for (i = 0, j = 0; i < ctx->limit_vars->nelts && j < octx->limit_vars->nelts;
			    i++, j++) 
	    {    
		    if (ngx_strcmp(v1[i].var.data, v2[j].var.data) != 0) { 
			    ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
					    "limit_req \"%V\" uses the \"%V\" variable "
					    "while previously it used the \"%V\" variable",
					    &shm_zone->shm.name, &v1[i].var,
					    &v2[j].var);
			    return NGX_ERROR;
		    }    
	    }    


	    ctx->sh = octx->sh;
	    ctx->shpool = octx->shpool;

	    return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;

        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_http_limit_proxy_req_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_http_limit_proxy_req_rbtree_insert_value);

    ngx_queue_init(&ctx->sh->queue);

    len = sizeof(" in limit_req zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in limit_req zone \"%V\"%Z",
                &shm_zone->shm.name);

    ctx->shpool->log_nomem = 0;

    return NGX_OK;
}


static void *
ngx_http_limit_proxy_req_create_conf(ngx_conf_t *cf)
{
    ngx_http_limit_proxy_req_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_proxy_req_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->limits.elts = NULL;
     */

    //reload conf
    if(conf->limits.nelts  != 0 ){
    	ngx_array_destroy(&conf->limits);
    }

    conf->limit_log_level = NGX_CONF_UNSET_UINT;
    conf->status_code = NGX_CONF_UNSET_UINT;

    return conf;
}


static char *
ngx_http_limit_proxy_req_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_limit_proxy_req_conf_t *prev = parent;
    ngx_http_limit_proxy_req_conf_t *conf = child;

    if (conf->limits.elts == NULL) {
        conf->limits = prev->limits;
    }

    ngx_conf_merge_uint_value(conf->limit_log_level, prev->limit_log_level,
                              NGX_LOG_ERR);

    conf->delay_log_level = (conf->limit_log_level == NGX_LOG_INFO) ?
                                NGX_LOG_INFO : conf->limit_log_level + 1;

    ngx_conf_merge_uint_value(conf->status_code, prev->status_code,
                              NGX_HTTP_SERVICE_UNAVAILABLE);

    return NGX_CONF_OK;
}


static char *
ngx_http_limit_proxy_req_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                    *p;
    size_t                     len;
    ssize_t                    size;
    ngx_str_t                 *value, name, s;
    ngx_int_t                  rate, scale;
    ngx_uint_t                 i;
    ngx_array_t                *variables;
    ngx_shm_zone_t            *shm_zone;
    value = cf->args->elts;
  
    ngx_http_limit_proxy_req_ctx_t      *ctx;
    ngx_http_limit_proxy_req_variable_t *v;
  
    ctx = NULL;
    size = 0;
    rate = 1;
    scale = 1;
    name.len = 0;

    variables = ngx_array_create(cf->pool, 2, sizeof(ngx_http_limit_proxy_req_variable_t));

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            name.data = value[i].data + 5;

            p = (u_char *) ngx_strchr(name.data, ':');

            if (p == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            name.len = p - name.data;

            s.data = p + 1;
            s.len = value[i].data + value[i].len - s.data;

            size = ngx_parse_size(&s);

            if (size == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            if (size < (ssize_t) (8 * ngx_pagesize)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "zone \"%V\" is too small", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rate=", 5) == 0) {

            len = value[i].len;
            p = value[i].data + len - 3;

            if (ngx_strncmp(p, "r/s", 3) == 0) {
                scale = 1;
                len -= 3;

            } else if (ngx_strncmp(p, "r/m", 3) == 0) {
                scale = 60;
                len -= 3;
            }

            rate = ngx_atoi(value[i].data + 5, len - 5);
            if (rate <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (value[i].data[0] == '$') {

	    v = ngx_array_push(variables);
           
	    value[i].len--;
            value[i].data++;
	    
	    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_proxy_req_ctx_t));
            if (ctx == NULL) {
                return NGX_CONF_ERROR;
            }

            v->index = ngx_http_get_variable_index(cf, &value[i]);
            if (v->index == NGX_ERROR) {
                return NGX_CONF_ERROR;
            }

            v->var = value[i];

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (variables->nelts == 0){
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"limit variable\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (ctx == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "no variable is defined for %V \"%V\"",
                           &cmd->name, &name);
        return NGX_CONF_ERROR;
    }

    ctx->rate = rate * 1000 / scale;
    ctx->limit_vars = variables;
    shm_zone = ngx_shared_memory_add(cf, &name, size,
                                     &ngx_http_limit_proxy_req_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ctx = shm_zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%V \"%V\" is already bound to variable",
                           &cmd->name, &name);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_limit_proxy_req_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}


static char *
ngx_http_limit_proxy_req(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_limit_proxy_req_conf_t  *lrcf = conf;
    u_char                     *p;
    ngx_int_t                  rate, scale, len;
    ngx_int_t                    burst;
    ngx_str_t                   *value, s, lim_key;
    ngx_uint_t                   i, nodelay;
    ngx_shm_zone_t              *shm_zone;
    ngx_http_limit_proxy_req_limit_t  *limit, *limits;
    value = cf->args->elts;

    shm_zone = NULL;
    burst = 0;
    nodelay = 1;
    rate = 0;
    scale = 1;
    len = 0;
    p = NULL;
    lim_key.len = 0;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            shm_zone = ngx_shared_memory_add(cf, &s, 0,
                                             &ngx_http_limit_proxy_req_module);
            if (shm_zone == NULL) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "burst=", 6) == 0) {

            burst = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (burst <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid burst rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

	if (ngx_strncmp(value[i].data, "lim_key=",8) == 0) {	
		lim_key.len = value[i].len - 8;
		lim_key.data = value[i].data + 8;
		continue;
	}

	if (ngx_strncmp(value[i].data, "rate=", 5) == 0) {

            len = value[i].len;
            p = value[i].data + len - 3;

            if (ngx_strncmp(p, "r/s", 3) == 0) {
                scale = 1;
                len -= 3;

            } else if (ngx_strncmp(p, "r/m", 3) == 0) {
                scale = 60;
                len -= 3;
            }

            rate = ngx_atoi(value[i].data + 5, len - 5);
            if (rate <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

	/*
	 * as a proxy server ,delay no usable
        if (ngx_strcmp(value[i].data, "nodelay") == 0) {
            nodelay = 1;
            continue;
        }
	*/
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unknown limit_req_zone \"%V\"",
                           &shm_zone->shm.name);
        return NGX_CONF_ERROR;
    }

    if (lim_key.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "\"%V\" must have \"lim_key\" parameter",
			&cmd->name);
        return NGX_CONF_ERROR;
    }

    limits = lrcf->limits.elts;

    if (limits == NULL) {
        if (ngx_array_init(&lrcf->limits, cf->pool, 1,
                           sizeof(ngx_http_limit_proxy_req_limit_t))
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    }
/*
    for (i = 0; i < lrcf->limits.nelts; i++) {
        if (shm_zone == limits[i].shm_zone) {
            return "is duplicate";
        }
    }
*/
    limit = ngx_array_push(&lrcf->limits);
    if (limit == NULL) {
        return NGX_CONF_ERROR;
    }

   // u_char *start = lim_key.data;
    size_t tmp_len = lim_key.len;
    size_t key_len = lim_key.len;

    ngx_str_t tmp_key = lim_key;
    u_char *key_start = tmp_key.data;
    u_char *start = tmp_key.data;
  // remove flag '&'
    while(tmp_len)
    {
	u_char *p = (u_char *)ngx_strchr(start, '&');
	if(p == NULL)
	{
		tmp_key.data = ngx_cpymem(tmp_key.data, start, tmp_len);
		break;
	}

	size_t len = p - start;
	tmp_key.data = ngx_cpymem(tmp_key.data, start, len);
	tmp_len -= (len + 1);
	start = p + 1;
	key_len--;
    }
    
 //   ngx_strlow(key_start, key_start, key_len); 
    limit->shm_zone = shm_zone;
    limit->burst = burst * 1000;
    limit->nodelay = nodelay;
    limit->prate.lim_key.len = key_len;
    limit->prate.lim_key.data = key_start;
    limit->prate.rate = rate * 1000/scale;
/*
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "\"%V\"'s lim_key parameter:\"%V\"",    
			&cmd->name, &limit->prate.lim_key);

*/
    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_limit_proxy_req_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_proxy_req_handler;

    return NGX_OK;
}
