#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>

#include "elr_mpl.h"

#ifdef ELR_USE_THREAD
#include "elr_mtx.h"
#endif // ELR_USE_THREAD

/*���ڴ�ڵ㻮�ֳɶ���ڴ���Ƭʱ������ڴ���Ƭ�ĳߴ硣*/
/*����Ƭ�ߴ糬����ֵʱ��һ���ڵ���ֻ��һ����Ƭ��*/
#define ELR_MAX_SLICE_SIZE                 32768  /*32KB*/

/*���ڴ�ڵ㻮�ֳɶ���ڴ���Ƭʱ������ڴ���Ƭ����Ŀ*/
/*���ڴ�ڵ㻮�ֳɶ���ڴ���Ƭʱ������ڴ�ڵ��СΪ ELR_MAX_SLICE_COUNT*ELR_MAX_SLICE_SIZE */
/*ʵ����Ҫ���ڴ���ƬԽ��ʱ�����յ��ڴ���Ƭ����Խ�٣�*/
/*ͨ��һ����ʽ��֤���ڴ�ڵ�Ĵ��ͽ��Ƶ��� ELR_MAX_SLICE_COUNT*ELR_MAX_SLICE_SIZE*/
#define ELR_MAX_SLICE_COUNT                64   /*64*/

/*��ߴ��ڴ���е��ӳض������������Сʱ�´������ڴ�ص��ڴ���С�Ļ���*/
/*���´������ڴ�ص��ڴ���СӦ���Ǵ��������С��ELR_OVERRANGE_UNIT_SIZE����С������*/
#define ELR_OVERRANGE_UNIT_SIZE            1024  /*1KB*/

/*�Զ����黹�ڵ�ռ���ڴ������ϵͳ���ڴ�ռ����ֵ*/
/*��ͨ�����ڴ��������ڴ���������512MBʱ���ͷ��ڴ治������ͷ�*/
#define ELR_AUTO_FREE_NODE_THRESHOLD       536870912 /*512MB*/

#define ELR_ALIGN(size, boundary)     (((size) + ((boundary) - 1)) & ~((boundary) - 1)) 

/*! \brief memory node type.
 *
 */
typedef struct __elr_mem_node
{
    struct __elr_mem_pool       *owner;
    struct __elr_mem_node       *prev;
    struct __elr_mem_node       *next;
	/*���е��ڴ���Ƭ����ͷ*/
    struct __elr_mem_slice      *free_slice_head;
	/*���е��ڴ���Ƭ����β*/
    struct __elr_mem_slice      *free_slice_tail;
	/*����ʹ�õ�slice������*/
    size_t                       using_slice_count;
	/*ʹ�ù���slice������*/
    size_t                       used_slice_count;
    char                        *first_avail;
}
elr_mem_node;

typedef struct __elr_mem_slice
{
    struct __elr_mem_slice      *prev;
    struct __elr_mem_slice      *next;
	/*�ڴ���Ƭ�������ڴ�ڵ�*/
    elr_mem_node                *node;
	/*����Ƭ�ı�ǩ����ʼֵΪ0��ÿһ�δ��ڴ����ȡ���͹黹�����1*/
	int                          tag;
}
elr_mem_slice;

typedef struct __elr_mem_pool
{
    struct __elr_mem_pool       *parent;
    struct __elr_mem_pool       *first_child;
    struct __elr_mem_pool       *prev;
    struct __elr_mem_pool       *next;
	/*��ϸ��ڴ��������벻ͬ�ߴ��ڴ��������ڴ��*/
	struct __elr_mem_pool      **multi;
	/*multi�а������ڴ�ص�����*/
	int                          multi_count;
	/*ÿ��elr_mem_node������slice������*/
    size_t                       slice_count;
    size_t                       slice_size;
	size_t                       object_size;
	/*elr_mem_node���ֽ���*/
    size_t                       node_size;
	/*����elr_mem_node��ɵ�����*/
    elr_mem_node                *first_node;
	/*�ոմ�����elr_mem_node*/
    elr_mem_node                *newly_alloc_node;
	/*���е��ڴ���Ƭ����*/
    elr_mem_slice               *first_free_slice;
	/*����ָ�룬�����ǵ�ǰ������ڴ棬����Ƭ������ʱִ��*/
	elr_mpl_callback             on_slice_alloc;
	/*����ָ�룬�����ǵ�ǰ�ͷŵ��ڴ棬����Ƭ���ͷ�ʱִ��*/
	elr_mpl_callback             on_slice_free;
	/*���õ��ڴ���Ƭ����*/
	elr_mem_slice               *first_occupied_slice;
	/*���ɱ��ڴ�ض�����ڴ���Ƭ�ı�ǩ*/
	int                          slice_tag;
#ifdef ELR_USE_THREAD
    elr_mtx                                pool_mutex;
#endif // ELR_USE_THREAD
}
elr_mem_pool;


/*ȫ���ڴ��*/
static elr_mem_pool   g_mem_pool;
/*ȫ�ֶ�ߴ��ڴ��*/
static elr_mpl_t      g_multi_mem_pool;
/*�����ڴ��ռ�ݵ��ڴ�����*/
static size_t         g_occupation_size;

elr_mpl_t ELR_MPL_INITIALIZER = { NULL,0 };

/*ȫ���ڴ�����ü���*/
#ifdef ELR_USE_THREAD
static elr_atomic_t     g_mpl_refs = ELR_ATOMIC_ZERO;
#else
static long           g_mpl_refs = 0;
#endif // ELR_USE_THREAD


/*Ϊ�ڴ������һ���ڴ�ڵ�*/
void                      _elr_alloc_mem_node(elr_mem_pool *pool);
/*�ͷ��ڴ�ڵ�*/
void                      _elr_free_mem_node(elr_mem_node* node);
/*���ڴ�صĸոմ������ڴ�ڵ��з���һ���ڴ���Ƭ*/
elr_mem_slice*            _elr_slice_from_node(elr_mem_pool *pool);
/*���ڴ���з���һ���ڴ���Ƭ���÷��������������������*/
elr_mem_slice*            _elr_slice_from_pool(elr_mem_pool *pool);
/*�����ڴ�أ�inner��ʾ�Ƿ��ǵݹ��ڲ����ã�lock_this�Ƿ���Ҫ������ǰ���ͷŵ��ڴ��*/
void                      _elr_mpl_destory(elr_mem_pool *pool, int inner, int lock_this);

/*
** ��ʼ���ڴ�أ��ڲ�����һ��ȫ���ڴ�ء�
** �÷������Ա��ظ����á�
** ����ڴ��ģ���Ѿ���ʼ���������������ü���Ȼ�󷵻ء�
*/
ELR_MPL_API int elr_mpl_init()
{
#ifdef ELR_USE_THREAD
	elr_counter_t refs = elr_atomic_inc(&g_mpl_refs);
	if(refs == 1)
	{
#else
	g_mpl_refs++;
	if(g_mpl_refs == 1)
	{
#endif // ELR_USE_THREAD
		g_occupation_size = 0;
		g_mem_pool.parent = NULL;
		g_mem_pool.first_child = NULL;
		g_mem_pool.prev = NULL;
		g_mem_pool.next = NULL;
		g_mem_pool.multi = NULL;
		g_mem_pool.multi_count = 0;
		g_mem_pool.object_size = sizeof(elr_mem_pool);
		g_mem_pool.slice_size = ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int))
			+ ELR_ALIGN(sizeof(elr_mem_pool),sizeof(int));
		g_mem_pool.slice_count = ELR_MAX_SLICE_COUNT;
		g_mem_pool.node_size = g_mem_pool.slice_size*g_mem_pool.slice_count 
			+ ELR_ALIGN(sizeof(elr_mem_node),sizeof(int));
		g_mem_pool.first_node = NULL;
		g_mem_pool.newly_alloc_node = NULL;
		g_mem_pool.first_free_slice = NULL;
		g_mem_pool.on_slice_alloc = NULL;
		g_mem_pool.on_slice_free = NULL;
		g_mem_pool.first_occupied_slice = NULL;
		g_mem_pool.slice_tag = 0;

#ifdef ELR_USE_THREAD
		if(elr_mtx_init(&g_mem_pool.pool_mutex) == 0)
		{
			elr_atomic_dec(&g_mpl_refs);
			return 0;
		}
		g_multi_mem_pool = elr_mpl_create_multi_2(NULL, NULL, NULL, 13, 64, 98, 128, 192, 256, 384, 512, 768, 1024, 1280, 1536, 1792, 2048);
		if (g_multi_mem_pool.pool == NULL)
		{
			elr_atomic_dec(&g_mpl_refs);
			return 0;
		}
#else
		g_multi_mem_pool = elr_mpl_create_multi_2(NULL, NULL, NULL, 13, 64, 98, 128, 192, 256, 384, 512, 768, 1024, 1280, 1536, 1792, 2048);
		if (g_multi_mem_pool.pool == NULL)
		{
			g_mpl_refs--;
			return 0;
		}
#endif // ELR_USE_THREAD
	}

	return 1;
}

/*
** ����һ���ڴ�أ���ָ�������䵥Ԫ��С��
** ��һ��������ʾ���ڴ�أ������ΪNULL����ʾ�������ڴ�صĸ��ڴ����ȫ���ڴ�ء�
*/
ELR_MPL_API elr_mpl_t elr_mpl_create(elr_mpl_ht fpool,size_t obj_size)
{
	return elr_mpl_create_ex(fpool,obj_size,NULL,NULL);
}


ELR_MPL_API elr_mpl_t elr_mpl_create_ex(elr_mpl_ht fpool,
										size_t obj_size,
										elr_mpl_callback on_alloc,
										elr_mpl_callback on_free)
{
	elr_mpl_t      mpl = ELR_MPL_INITIALIZER;
	elr_mem_slice *pslice = NULL;
	elr_mem_pool  *pool = NULL;

	assert(fpool==NULL || elr_mpl_avail(fpool)!=0);

	if((pslice = _elr_slice_from_pool(&g_mem_pool)) == NULL)
		return mpl;
	pool = (elr_mem_pool*)((char*)pslice
		+ ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));

#ifdef ELR_USE_THREAD	
	if (elr_mtx_init(&pool->pool_mutex) == 0)
	{
		elr_mpl_free(pool);
		return mpl;
	}
#endif // ELR_USE_THREAD
	pool->slice_tag = pslice->tag;
	pool->first_child = NULL;
	pool->parent = fpool == NULL ? &g_mem_pool : (elr_mem_pool*)fpool->pool;
	pool->multi = NULL;
	pool->multi_count = 0;
	pool->object_size = obj_size;
	pool->slice_size = ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int))
		+ ELR_ALIGN(obj_size,sizeof(int));
	if(pool->slice_size < ELR_MAX_SLICE_SIZE)
		pool->slice_count = ELR_MAX_SLICE_COUNT 
		- pool->slice_size*(ELR_MAX_SLICE_COUNT-1)/ELR_MAX_SLICE_SIZE;
	else
		pool->slice_count = 1;
	pool->node_size = pool->slice_size*pool->slice_count 
		+ ELR_ALIGN(sizeof(elr_mem_node),sizeof(int));
	pool->first_node = NULL;
	pool->newly_alloc_node = NULL;
	pool->first_free_slice = NULL;
	pool->on_slice_alloc = on_alloc;
	pool->on_slice_free = on_free;
	pool->first_occupied_slice = NULL;

#ifdef ELR_USE_THREAD
	elr_mtx_lock(&pool->parent->pool_mutex);
#endif // ELR_USE_THREAD
	pool->prev = NULL;
	pool->next = pool->parent->first_child;
	if(pool->next != NULL)
		pool->next->prev = pool;
	pool->parent->first_child = pool;
#ifdef ELR_USE_THREAD
	elr_mtx_unlock(&pool->parent->pool_mutex);
#endif // ELR_USE_THREAD

	mpl.pool = pool;
	mpl.tag = pool->slice_tag;

	return mpl;
}

ELR_MPL_API elr_mpl_t elr_mpl_create_multi_1(elr_mpl_ht fpool, elr_mpl_callback on_alloc, elr_mpl_callback on_free, int obj_size_count, size_t* obj_size)
{
	elr_mpl_t      first_mpl = ELR_MPL_INITIALIZER;
	elr_mpl_t      mpl = ELR_MPL_INITIALIZER;
	elr_mem_pool **multi_pool = NULL;

	int i = 0;
	int j = 0;
	int valid = 1;

	multi_pool = malloc(obj_size_count * sizeof(elr_mem_pool*));
	if (multi_pool == NULL)
		return ELR_MPL_INITIALIZER;

	for (i = 0; i < obj_size_count; i++)
	{
		mpl = elr_mpl_create_ex(fpool, obj_size[i], on_alloc, on_free);
		if (mpl.pool == NULL)
		{
			valid = 0;
			break;
		}
		multi_pool[i] = (elr_mem_pool*)mpl.pool;
		if (i == 0)
		{
			first_mpl = mpl;
			multi_pool[i]->multi = multi_pool;
			multi_pool[i]->multi_count = obj_size_count;
		}
	}

	if (valid == 1)
	{
		if (g_multi_mem_pool.pool == NULL)
		{
			g_multi_mem_pool = first_mpl;
		}
		multi_pool[0]->multi = (elr_mem_pool**)elr_mpl_alloc_multi(&g_multi_mem_pool, obj_size_count * sizeof(elr_mem_pool*));
		if (multi_pool[0]->multi != NULL)
		{
			memcpy(multi_pool[0]->multi, multi_pool, obj_size_count * sizeof(elr_mem_pool*));
		}
		else
		{
			valid = 0;
		}
	}

	if (valid == 0)
	{
		first_mpl = ELR_MPL_INITIALIZER;
		for (j = 0; j < i; j++)
		{
			_elr_mpl_destory(multi_pool[j], 0, 0);
		}
	}

	free(multi_pool);

	return first_mpl;
}

ELR_MPL_API elr_mpl_t elr_mpl_create_multi_2(elr_mpl_ht fpool, elr_mpl_callback on_alloc, elr_mpl_callback on_free, int obj_size_count, ...)
{
	elr_mpl_t      mpl = ELR_MPL_INITIALIZER;

	int i = 0;
	size_t* obj_size;
	va_list obj_size_list; 

	obj_size = (size_t*)malloc(obj_size_count * sizeof(size_t));

	if (obj_size == NULL)
	{
		return ELR_MPL_INITIALIZER;
	}

	va_start(obj_size_list, obj_size_count);
	for (i = 0; i < obj_size_count; i++)
	{
		obj_size[i] = va_arg(obj_size_list, int);
	}
	va_end(obj_size_list);

	mpl =  elr_mpl_create_multi_1(fpool, on_alloc, on_free, obj_size_count, obj_size);

	free(obj_size);

	return mpl;
}


/*
** �ж��ڴ���Ƿ�����Ч�ģ�һ���ڴ�����ɺ��������á�
** ����0��ʾ��Ч
** pool����ΪNULL
*/
ELR_MPL_API int  elr_mpl_avail(elr_mpl_ht hpool)
{
	int              ret = 1;
	elr_mem_slice   *pslice = NULL;
	elr_mem_pool    *pool = NULL;

	if(hpool->pool == NULL)
	{
		ret = 0;
	}
	else
	{
		pool = (elr_mem_pool*)hpool->pool;
		pslice = (elr_mem_slice*)((char*)(hpool->pool) 
			- ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));
		if (hpool->tag != pool->slice_tag 
			|| hpool->tag != pslice->tag)
			ret = 0;
	}

	return ret;
}

/*
** ���ڴ���������ڴ档
*/
ELR_MPL_API void*  elr_mpl_alloc(elr_mpl_ht hpool)
{
	elr_mem_slice *pslice = NULL;
	elr_mem_pool  *pool = NULL;

	assert(hpool != NULL  && elr_mpl_avail(hpool)!=0);

	pool = (elr_mem_pool*)hpool->pool;
	pslice = _elr_slice_from_pool(pool);

    if(pslice == NULL)
        return NULL;
    else
	{
		char *mem = (char*)pslice 
			+ ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int));
		if (pool->on_slice_alloc != NULL)
			pool->on_slice_alloc(mem);

		return mem;
	}
}

ELR_MPL_API void * elr_mpl_alloc_multi(elr_mpl_ht hpool, size_t size)
{
	void*          mem = NULL;
	elr_mpl_t      alloc_mpl = ELR_MPL_INITIALIZER;
	elr_mem_slice *slice = NULL;
	elr_mem_pool  *pool = NULL;
	elr_mem_pool  *parent_pool = NULL;
	elr_mem_pool  *child_pool = NULL;
	elr_mem_slice *pool_slice = NULL;
	elr_mem_pool  *alloc_pool = NULL;
	int i = 0;

	assert(hpool == NULL  || elr_mpl_avail(hpool) != 0);

	if (hpool == NULL)
		hpool = &g_multi_mem_pool;
	pool = (elr_mem_pool*)hpool->pool;

	assert(pool->multi != NULL);

	parent_pool = pool->multi[pool->multi_count - 1];
	pool_slice = (elr_mem_slice*)((char*)(pool)
		-ELR_ALIGN(sizeof(elr_mem_slice), sizeof(int)));

#ifdef ELR_USE_THREAD
	elr_mtx_lock(&pool->pool_mutex);
#endif // ELR_USE_THREAD

	if (hpool->tag != pool->slice_tag 
		|| pool->slice_tag != pool_slice->tag)
		return NULL;

	for (i = 0; i < pool->multi_count; i++)
	{
		if (pool->multi[i]->object_size >= size)
		{
			alloc_pool = pool->multi[i];
			break;
		}
	}

	if (alloc_pool == NULL)
	{		
		child_pool = parent_pool->first_child;
		while (child_pool != NULL)
		{
			if (child_pool->object_size >= size)
			{
				alloc_pool = child_pool;
				break;
			}
			child_pool = child_pool->next;
		}
	}

	if (alloc_pool == NULL)
	{
		size = ELR_OVERRANGE_UNIT_SIZE*((size + ELR_OVERRANGE_UNIT_SIZE - 1) / ELR_OVERRANGE_UNIT_SIZE);
		alloc_mpl.pool = parent_pool;
		alloc_mpl.tag = parent_pool->slice_tag;
		alloc_mpl = elr_mpl_create_ex(&alloc_mpl, size, parent_pool->on_slice_alloc, parent_pool->on_slice_free);
		alloc_pool = (elr_mem_pool*)alloc_mpl.pool;
	}
	else
	{
		alloc_mpl.pool = alloc_pool;
		alloc_mpl.tag = alloc_pool->slice_tag;
	}

	if (alloc_pool != NULL)
	{
		mem = elr_mpl_alloc(&alloc_mpl);
	}

#ifdef ELR_USE_THREAD
	elr_mtx_unlock(&pool->pool_mutex);
#endif // ELR_USE_THREAD
	return mem;
}


/*
** ��ȡ���ڴ����������ڴ��ĳߴ硣
*/
ELR_MPL_API size_t elr_mpl_size(void* mem)
{
    elr_mem_slice *slice = (elr_mem_slice*)((char*)mem
		- ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));
    return slice->node->owner->object_size;
}

/*
** ���ڴ��˻ظ��ڴ�ء�ִ�и÷���Ҳ���ܽ��ڴ��˻ظ�ϵͳ��
*/
ELR_MPL_API void  elr_mpl_free(void* mem)
{
    elr_mem_slice *slice = (elr_mem_slice*)((char*)mem 
		- ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));
    elr_mem_node*  node = slice->node;
    elr_mem_pool*  pool = node->owner;
	elr_mem_slice *pool_slice = (elr_mem_slice*)((char*)(pool)
		-ELR_ALIGN(sizeof(elr_mem_slice), sizeof(int)));

#ifdef ELR_USE_THREAD
    elr_mtx_lock(&pool->pool_mutex);
#endif // ELR_USE_THREAD

	if (pool_slice->tag != pool->slice_tag)
		return;

	slice->tag++;
	node->using_slice_count--;
	if (pool->on_slice_free != NULL)
	{
		pool->on_slice_free(mem);
	}

	if (slice->next != NULL)
		slice->next->prev = slice->prev;

	if (slice->prev != NULL)
		slice->prev->next = slice->next;
	else
		pool->first_occupied_slice = slice->next;

	if (node->using_slice_count == 0
		&& g_occupation_size >= ELR_AUTO_FREE_NODE_THRESHOLD)
	{
		_elr_free_mem_node(node);
	}
	else
	{
		if (node->free_slice_head == NULL)
		{
			node->free_slice_head = slice;
			node->free_slice_tail = slice;
			slice->prev = NULL;
			slice->next = pool->first_free_slice;
			if (pool->first_free_slice != NULL)
				pool->first_free_slice->prev = slice;
			pool->first_free_slice = slice;
		}
		else
		{
			slice->next = node->free_slice_tail->next;
			if (node->free_slice_tail->next != NULL)
				node->free_slice_tail->next->prev = slice;
			node->free_slice_tail->next = slice;
			slice->prev = node->free_slice_tail;
			node->free_slice_tail = slice;
		}
	}

#ifdef ELR_USE_THREAD
    elr_mtx_unlock(&pool->pool_mutex);
#endif // ELR_USE_THREAD
    return;
}

/*
** �����ڴ�غ������ڴ�ء�
*/
ELR_MPL_API void elr_mpl_destroy(elr_mpl_ht hpool)
{
    elr_mem_pool  *pool = NULL;
	elr_mem_slice *pslice = NULL;
	int            j = 0;

	assert(hpool!=NULL && elr_mpl_avail(hpool)!=0);
	pool = (elr_mem_pool*)hpool->pool;
	assert(pool->parent != NULL);

#ifdef ELR_USE_THREAD
    elr_mtx_lock(&pool->pool_mutex);
#endif // ELR_USE_THREAD

	pslice = (elr_mem_slice*)((char*)pool 
		- ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));

	if (hpool->tag == pool->slice_tag 
		&&  hpool->tag == pslice->tag)
	{
		if (pool->multi != NULL)
		{
			for (j = 0; j < pool->multi_count; j++)
			{
				_elr_mpl_destory(pool->multi[j], 0, 0);
			}
		}
		else
		{
			_elr_mpl_destory(pool, 0, 1);
		}
	}

	hpool->pool = NULL;
	hpool->tag = 0;
#ifdef ELR_USE_THREAD
    elr_mtx_unlock(&pool->pool_mutex);
#endif // ELR_USE_THREAD    
}

/*
** ��ֹ�ڴ��ģ�飬������ȫ���ڴ�ؼ������ڴ�ء�
** �����д����������ڴ�����û����ʾ���ͷţ�ִ�д˲�����Ҳ�ᱻ�ͷš�
*/
ELR_MPL_API void elr_mpl_finalize()
{
#ifdef ELR_USE_THREAD
	elr_counter_t   refs = 1;
    elr_mtx_lock(&g_mem_pool.pool_mutex);
	refs = elr_atomic_dec(&g_mpl_refs);
    if(refs == 0)
    {
#else
	g_mpl_refs--;
	if(g_mpl_refs == 0)
	{
#endif // ELR_USE_THREAD
		_elr_mpl_destory(&g_mem_pool,0,1);
    }

#ifdef ELR_USE_THREAD
    elr_mtx_unlock(&g_mem_pool.pool_mutex);
#endif // ELR_USE_THREAD
}


void _elr_alloc_mem_node(elr_mem_pool *pool)
{
    elr_mem_node* pnode = (elr_mem_node*)malloc(pool->node_size);
    if(pnode == NULL)
        return;

	g_occupation_size += pool->node_size;
    pool->newly_alloc_node = pnode;
    pnode->owner = pool;
    pnode->first_avail = (char*)pnode
		+ ELR_ALIGN(sizeof(elr_mem_node),sizeof(int));

	pnode->free_slice_head = NULL;
    pnode->free_slice_tail = NULL;
    pnode->used_slice_count = 0;
	pnode->using_slice_count = 0;
	pnode->prev = NULL;

    if(pool->first_node == NULL)
    {
        pool->first_node = pnode;
        pnode->next = NULL;		
    }
    else
    {
        pnode->next = pool->first_node;
        pool->first_node->prev = pnode;
        pool->first_node = pnode;
    }
}

/*�Ƴ�һ��δʹ�õ�NODE������0��ʾû���Ƴ�*/
void _elr_free_mem_node(elr_mem_node* pnode)
{
	assert(pnode->using_slice_count == 0);

	if (pnode->free_slice_tail != NULL 
		&& pnode->free_slice_tail->next != NULL)
		pnode->free_slice_tail->next->prev = pnode->free_slice_head->prev;

	if (pnode->free_slice_head != NULL 
		&& pnode->free_slice_head->prev != NULL)
		pnode->free_slice_head->prev->next = pnode->free_slice_tail->next;

	if (pnode->owner->first_free_slice != NULL
		&& pnode->owner->first_free_slice == pnode->free_slice_head)
		pnode->owner->first_free_slice = pnode->free_slice_tail->next;

	if (pnode->owner->newly_alloc_node == pnode)
		pnode->owner->newly_alloc_node = NULL;

	if (pnode->next != NULL)
		pnode->next->prev = pnode->prev;

	if (pnode->prev != NULL)
		pnode->prev->next = pnode->next;
	else
		pnode->owner->first_node = pnode->next;

	g_occupation_size -= pnode->owner->node_size;
	free(pnode);
}

elr_mem_slice* _elr_slice_from_node(elr_mem_pool *pool)
{
    elr_mem_slice *pslice = NULL;

    if(pool->newly_alloc_node != NULL)
    {
        pool->newly_alloc_node->used_slice_count++;
		pool->newly_alloc_node->using_slice_count++;
        pslice = (elr_mem_slice*)pool->newly_alloc_node->first_avail;
		memset(pslice,0,pool->slice_size);
		pslice->next = NULL;
		pslice->prev = NULL;
		pslice->tag = 0;
        pool->newly_alloc_node->first_avail += pool->slice_size;
        pslice->node = pool->newly_alloc_node;
        if(pool->newly_alloc_node->used_slice_count == pool->slice_count)
            pool->newly_alloc_node = NULL;
    }

    return pslice;
}

/*
** ���ڴ���������ڴ档
*/
elr_mem_slice* _elr_slice_from_pool(elr_mem_pool* pool)

{
    elr_mem_slice *slice = NULL;
	elr_mem_slice *pool_slice = (elr_mem_slice*)((char*)(pool)
		-ELR_ALIGN(sizeof(elr_mem_slice), sizeof(int)));

	assert(pool != NULL);

#ifdef ELR_USE_THREAD
    elr_mtx_lock(&pool->pool_mutex);
#endif // ELR_USE_THREAD

	if (pool_slice->tag != pool->slice_tag)
		return NULL;

    if(pool->first_free_slice != NULL)
    {
        slice = pool->first_free_slice;
		pool->first_free_slice = slice->next;
		slice->node->free_slice_head = NULL;
		if (pool->first_free_slice != NULL)
		{
			pool->first_free_slice->prev = NULL;
			if(slice->next->node == slice->node)
				slice->node->free_slice_head = slice->next;
		}
		
		if (slice->node->free_slice_head == NULL)
			slice->node->free_slice_tail = NULL;

		slice->next = NULL;
		slice->prev = NULL;
		slice->tag++;
		slice->node->using_slice_count++;
    }
    else
    {
        if(pool->newly_alloc_node == NULL)
            _elr_alloc_mem_node(pool);
        slice = _elr_slice_from_node(pool);
    }

	if (slice != NULL)
	{
		slice->prev = NULL;
		slice->next = pool->first_occupied_slice;
		if (pool->first_occupied_slice != NULL)
			pool->first_occupied_slice->prev = slice;
		pool->first_occupied_slice = slice;
	}

#ifdef ELR_USE_THREAD
    elr_mtx_unlock(&pool->pool_mutex);
#endif // ELR_USE_THREAD

	return slice;
}


void _elr_mpl_destory(elr_mem_pool *pool, int inner, int lock_this)
{
    elr_mem_pool   *temp_pool = NULL;
    elr_mem_node   *temp_node = NULL;
	size_t                  index = 0;

#ifdef ELR_USE_THREAD
	if (inner == 1 && lock_this == 1)
        elr_mtx_lock(&(pool->pool_mutex));
	if (inner == 0 && pool->parent != NULL)
		elr_mtx_lock(&(pool->parent->pool_mutex));
#endif // ELR_USE_THREAD	

	if (pool->next != NULL)
		pool->next->prev = pool->prev;
	if (pool->prev != NULL)
		pool->prev->next = pool->next;

	if (pool->prev == NULL && pool->parent != NULL)
		pool->parent->first_child = pool->next;

#ifdef ELR_USE_THREAD
	if (inner == 0 && pool->parent != NULL)
	    elr_mtx_unlock(&(pool->parent->pool_mutex));
#endif // ELR_USE_THREAD

    while((temp_pool = pool->first_child) != NULL)
    {
		_elr_mpl_destory(temp_pool, 1, lock_this);
	}

#ifdef ELR_USE_THREAD
	if (inner == 1 && lock_this == 1)
		elr_mtx_unlock(&(pool->pool_mutex));
	elr_mtx_finalize(&pool->pool_mutex);
#endif // ELR_USE_THREAD

	if (pool->on_slice_free != NULL)
	{
		elr_mem_slice* temp_slice = pool->first_occupied_slice;
		while(temp_slice != NULL)
		{
			pool->first_occupied_slice = temp_slice->next;
			pool->on_slice_free((char*)temp_slice 
				+ ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));
			temp_slice = pool->first_occupied_slice;
		}		
	}

	temp_node = pool->first_node;
	while(temp_node != NULL)
	{		
		pool->first_node = temp_node->next;
		free(temp_node);
		temp_node = pool->first_node ;
	}

	pool->parent = NULL;
	pool->slice_tag = -1;
	if(pool != g_multi_mem_pool.pool && pool->multi != NULL)
		elr_mpl_free(pool->multi);

	/*������Ǹ��ڵ�*/
	if(pool != &g_mem_pool)
		elr_mpl_free(pool);
}
