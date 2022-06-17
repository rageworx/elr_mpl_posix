#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <pthread.h>

#include "elr_mpl_posix.h"

/*The size of the largest memory slice*/
/** max memory node size. it can be changed to fit the memory consume more.  */
#define ELR_MAX_SLICE_SIZE              32768  /*32KB*/
/*The maximum number of memory slices when dividing a memory node into multiple memory slices*/
/*The maximum memory node size when dividing a memory node into multiple memory slices is ELR_MAX_SLICE_COUNT*ELR_MAX_SLICE_SIZE */
/* When the actual memory slice required is larger, the final number of memory slices will be smaller, */
/*The size of the memory node is guaranteed to be approximately equal to ELR_MAX_SLICE_COUNT*ELR_MAX_SLICE_SIZE*/
#define ELR_MAX_SLICE_COUNT             64     /*64*/

/*The cardinality of the memory block size of the newly created memory pool when none of the sub-pools in the multi-size memory pool meet the requested size*/
/* That is, the memory block size of the newly created memory pool should be the smallest integer multiple of ELR_OVERRANGE_UNIT_SIZE larger than the application size*/
#define ELR_OVERRANGE_UNIT_SIZE         1024  /*1KB*/

/*Automatically occupy memory for the returned node to the memory usage threshold of the operating system*/
/* When the total amount of memory requested through this memory pool is less than 512MB, releasing the memory will not really release*/
#define ELR_AUTO_FREE_NODE_THRESHOLD    536870912 /*512MB*/

#define ELR_ALIGN(size, boundary)       (((size) + ((boundary) - 1)) & ~((boundary) - 1)) 

/*! \brief memory node type.
 *
 */
typedef struct __elr_mem_node
{
    struct __elr_mem_pool       *owner;
    struct __elr_mem_node       *prev;
    struct __elr_mem_node       *next;
    /*Free memory slice linked list header*/
    struct __elr_mem_slice      *free_slice_head;
    /*Free memory slice linked list tail*/
    struct __elr_mem_slice      *free_slice_tail;
    /*the number of slices in use*/
    size_t                       using_slice_count;
    /*The number of slices used*/
    size_t                       used_slice_count;
    char                        *first_avail;
}
elr_mem_node;

typedef struct __elr_mem_slice
{
    struct __elr_mem_slice      *prev;
    struct __elr_mem_slice      *next;
    /*The memory node to which the memory slice belongs*/
    elr_mem_node                *node;
    /*The label of the inner slice, the initial value is 0, and it will be incremented by 1 every time it is taken out and returned from the memory pool*/
    int                          tag;
}
elr_mem_slice;

typedef struct __elr_mem_pool
{
    struct __elr_mem_pool       *parent;
    struct __elr_mem_pool       *first_child;
    struct __elr_mem_pool       *prev;
    struct __elr_mem_pool       *next;
	/* Cooperate with this memory pool to complete the application for other memory pools of different size memory blocks*/
	struct __elr_mem_pool      **multi;
	/*Number of memory pools included in multi*/
	int                          multi_count;
	/*The number of slices contained in each elr_mem_node*/
    size_t                       slice_count;
    size_t                       slice_size;
    size_t                       object_size;
    size_t                       node_size;
    /*A linked list of all elr_mem_nodes*/
    elr_mem_node                *first_node;
    /*Just created elr_mem_node*/
    elr_mem_node                *newly_alloc_node;
    /*Linked list of free memory slices*/
    elr_mem_slice               *first_free_slice;
    /*Function pointer, the parameter is the currently allocated memory, executed when the slice is allocated*/
    elr_mpl_callback             on_slice_alloc;
    /*Function pointer, the parameter is the currently freed memory, executed when the slice is freed*/
    elr_mpl_callback             on_slice_free;
    /*The linked list of memory slices in use, if on_slice_free is NULL, this member is not used*/
    elr_mem_slice               *first_occupied_slice;
    /* The label of the memory slice that holds the object of this memory pool */
    int                          slice_tag;
    /*Whether the synchronization lock is created*/
	int                          sync;
    pthread_mutex_t              pool_mutex;
}
elr_mem_pool;


/*global memory pool*/
static elr_mem_pool     g_mem_pool;
/*Global multi-size memory pool*/
static elr_mpl_t        g_multi_mem_pool;
/* The total amount of memory occupied by all memory pools */
static size_t           g_occupation_size = 0;

elr_mpl_t ELR_MPL_INITIALIZER = { NULL,0 };

/*Global memory pool reference count*/
static long             g_mpl_refs = 0;
static pthread_mutex_t  g_mpl_refs_mtx = PTHREAD_MUTEX_INITIALIZER;

/*Create a memory pool and specify the allocation unit size, whether sync is performed with synchronization support. */
elr_mem_pool*       _elr_mpl_create(elr_mem_pool* pool, 
	                                size_t obj_size, 
	                                elr_mpl_callback on_alloc, 
	                                elr_mpl_callback on_free, 
	                                int sync);
/*Create a memory pool from which you can apply for memory blocks of different sizes, whether sync is executed with synchronization support. */
elr_mem_pool*       _elr_mpl_create_multi(elr_mem_pool* pool,
	                                      int obj_size_count,
	                                      size_t* obj_size,
	                                      elr_mpl_callback on_alloc, 
	                                      elr_mpl_callback on_free, 
	                                      int sync);
/* Determine if the memory pool is valid */
int                 _elr_mpl_avail(elr_mem_pool* pool);
/* Apply for a memory node for the memory pool */
void                 _elr_alloc_mem_node(elr_mem_pool *pool);
/*Remove an unused NODE, return 0 for no removal*/
void                _elr_free_mem_node(elr_mem_node* node);
/*Allocate a memory slice in the just created memory node of the memory pool*/
elr_mem_slice*      _elr_slice_from_node(elr_mem_pool *pool);
/*Allocate a memory slice in the memory pool, this method will call the above two methods*/
elr_mem_slice*      _elr_slice_from_pool(elr_mem_pool *pool);
/*Destroy the memory pool, inter indicates whether it is an internal call*/
void                _elr_mpl_destory(elr_mem_pool *pool, int inner, int lock_this);

static long elr_atomic_inc( long* p )
{
    if ( p != NULL )
    {
        pthread_mutex_lock( &g_mpl_refs_mtx );
        *p += 1;
        pthread_mutex_unlock( &g_mpl_refs_mtx);
        return *p;
    }
    
    return 0;
}

static long elr_atomic_dec( long* p )
{
    if ( p != NULL )
    {
        pthread_mutex_lock( &g_mpl_refs_mtx );
        if ( *p > 0 ) *p -= 1;
        pthread_mutex_unlock( &g_mpl_refs_mtx);
        return *p;
    }
    
    return -1;
}

/*** Initialize the memory pool and create a global memory pool internally.
** This method can be called repeatedly.
** If the memory pool module has already been initialized, just increment the reference count and return.
*/
ELR_MPL_API int elr_mpl_init()
{
	int obj_size_count = 13;
	size_t obj_size[13] = { 64, 98, 128, 192, 256, 384, 512, 768, 1024, 1280, 1536, 1792, 2048 };
    long refs = elr_atomic_inc(&g_mpl_refs);
    if(refs == 1)
    {
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
		g_mem_pool.sync = 1;
        
        if( pthread_mutex_init( &g_mem_pool.pool_mutex, NULL ) != 0 )
        {
            elr_atomic_dec( &g_mpl_refs );
            g_mem_pool.sync = 0;
            return 0;
        }

		g_multi_mem_pool = elr_mpl_create_multi_sync(NULL, obj_size_count, obj_size, NULL, NULL);
		if (g_multi_mem_pool.pool == NULL)
		{
			elr_atomic_dec(&g_mpl_refs);
            g_mem_pool.sync = 0;
			return 0;
		}
    }

    return 1;
}

/*
** Create a memory pool and specify the maximum allocation unit size.
** The first parameter represents the parent memory pool. If it is NULL, it means that the parent memory pool of the created memory pool is the global memory pool.
*/
ELR_MPL_API elr_mpl_t elr_mpl_create(elr_mpl_ht fpool,
                                  	 size_t obj_size,
                                  	 elr_mpl_callback on_alloc,
                                  	 elr_mpl_callback on_free)
{
	elr_mpl_t      mpl = ELR_MPL_INITIALIZER;
	elr_mem_pool  *pool = NULL;

	assert(fpool == NULL || elr_mpl_avail(fpool) != 0);

    elr_mem_pool* tpl = NULL;
    if ( fpool != NULL )
        tpl = (elr_mem_pool*)fpool->pool;

	pool = _elr_mpl_create( tpl, obj_size, on_alloc, on_free, 0);
	if (pool != NULL)
	{
		mpl.pool = pool;
		mpl.tag = pool->slice_tag;
    }

	return mpl;
}

/*
** Create a memory pool.
** The first parameter represents the parent memory pool. If it is NULL, it means that the parent memory pool of the created memory pool is the global memory pool.
** The second parameter represents the allocation unit size.
** The third parameter provides a pointer to a function that will be executed when the memory is freed.
*/
ELR_MPL_API elr_mpl_t elr_mpl_create_sync(elr_mpl_ht fpool,
                                        size_t obj_size,
                                        elr_mpl_callback on_alloc,
                                        elr_mpl_callback on_free)
{
    elr_mpl_t      mpl = ELR_MPL_INITIALIZER;
    elr_mem_pool  *pool = NULL;

#ifdef DEBUG
    assert(fpool==NULL || elr_mpl_avail(fpool)!=0);
#endif

    elr_mem_pool* tpl = NULL;
    if ( fpool != NULL )
        tpl = (elr_mem_pool*)fpool->pool;

	pool = _elr_mpl_create( tpl, obj_size, on_alloc, on_free, 1);
	if (pool != NULL)
	{
		mpl.pool = pool;
		mpl.tag = pool->slice_tag;
	}

        return mpl;
}

/*Create a memory pool and specify the allocation unit size, whether sync is performed with synchronization support. */
elr_mem_pool* _elr_mpl_create(elr_mem_pool* fpool,
	size_t obj_size,
	elr_mpl_callback on_alloc,
	elr_mpl_callback on_free,
	int sync)
{
	elr_mem_slice *pslice = NULL;
	elr_mem_pool  *pool = NULL;

	if ((pslice = _elr_slice_from_pool(&g_mem_pool)) == NULL)
		return NULL;
    
    pool = (elr_mem_pool*)((char*)pslice
        + ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));

    pool->sync = sync;
	if (sync == 1 && pthread_mutex_init(&pool->pool_mutex, NULL) != 0)
    {
        pool->sync = 0;
        elr_mpl_free(pool);
    	return NULL;
    }
    
	pool->slice_tag = pslice->tag;
	pool->first_child = NULL;
	pool->parent = fpool == NULL ? &g_mem_pool : fpool;
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

    if(pool->parent->sync == 1)
        pthread_mutex_lock(&pool->parent->pool_mutex);
        
    pool->prev = NULL;
    pool->next = pool->parent->first_child;
    if(pool->next != NULL)
        pool->next->prev = pool;
    pool->parent->first_child = pool;
    
	if (pool->parent->sync == 1)
		pthread_mutex_unlock(&pool->parent->pool_mutex);
    
    return pool;
}

/*Create a memory pool from which you can apply for memory blocks of different sizes, whether sync is executed with synchronization support. */
elr_mem_pool* _elr_mpl_create_multi(elr_mem_pool* fpool,
	int obj_size_count,
	size_t* obj_size,
	elr_mpl_callback on_alloc,
	elr_mpl_callback on_free,
	int sync)
{
	elr_mem_pool  *first_pool = NULL;
	elr_mem_pool  *pool = NULL;
	elr_mem_pool **multi_pool = NULL;

	int i = 0;
	int j = 0;
	int valid = 1;

	multi_pool = (elr_mem_pool**)malloc(obj_size_count * sizeof(elr_mem_pool*));
	if (multi_pool == NULL)
		return NULL;

	for (i = 0; i < obj_size_count; i++)
	{
		pool = _elr_mpl_create(fpool, obj_size[i], on_alloc, on_free, i == 0 ? sync : 0);
		if (pool == NULL)
		{
			valid = 0;
			break;
		}
		multi_pool[i] = (elr_mem_pool*)pool;
		if (i == 0)
		{
			first_pool = pool;
			pool->multi = multi_pool;
			pool->multi_count = obj_size_count;
		}
	}

	if (valid == 1)
	{
        //g_multi_mem_pool is also applied through this method,
        //But this method needs to rely on g_multi_mem_pool;
        //So if g_multi_mem_pool has not been applied for, let it be valid first.
		if (g_multi_mem_pool.pool == NULL)
		{
			g_multi_mem_pool.pool = first_pool;
			g_multi_mem_pool.tag = first_pool->slice_tag;
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
		first_pool = NULL;
		for (j = 0; j < i; j++)
		{
			_elr_mpl_destory(multi_pool[j], 0, 0);
		}
	}

	free(multi_pool);

	return first_pool;
}

ELR_MPL_API elr_mpl_t elr_mpl_create_multi(elr_mpl_ht fpool,
	int obj_size_count,
	size_t* obj_size,
	elr_mpl_callback on_alloc,
	elr_mpl_callback on_free)
{
	elr_mpl_t      mpl = ELR_MPL_INITIALIZER;
	elr_mem_pool  *pool = NULL;

	assert(fpool == NULL || elr_mpl_avail(fpool) != 0);

    elr_mem_pool* tpl = NULL;
    if ( fpool != NULL )
        tpl = (elr_mem_pool*)fpool->pool;

	pool = _elr_mpl_create_multi( tpl, obj_size_count, obj_size, on_alloc, on_free, 0);
	if (pool != NULL)
	{
		mpl.pool = pool;
		mpl.tag = pool->slice_tag;
	}

	return mpl;
}

ELR_MPL_API elr_mpl_t elr_mpl_create_multi_sync(elr_mpl_ht fpool,
	int obj_size_count,
	size_t* obj_size,
	elr_mpl_callback on_alloc,
	elr_mpl_callback on_free)
{
	elr_mpl_t      mpl = ELR_MPL_INITIALIZER;
	elr_mem_pool  *pool = NULL;

	assert(fpool == NULL || elr_mpl_avail(fpool) != 0);

    elr_mem_pool* tpl = NULL;
    if ( fpool != NULL )
        tpl = (elr_mem_pool*)fpool->pool;
	
	pool = _elr_mpl_create_multi( tpl, obj_size_count, obj_size, on_alloc, on_free, 1);
	if (pool != NULL)
	{
      mpl.pool = pool;
      mpl.tag = pool->slice_tag;
	}

    return mpl;
}

/*** To determine whether the memory pool is valid, it is generally called immediately after the creation is completed.
** return 0 for invalid
** pool cannot be NULL
*/
ELR_MPL_API int  elr_mpl_avail(elr_mpl_ht hpool)
{
    int              ret = 1;
    elr_mem_slice   *pslice = NULL;
	elr_mem_pool    *pool = NULL;

	assert(hpool != NULL);

    if(hpool->pool == NULL)
    {
        ret = 0;
    }
    else
    {
		pool = (elr_mem_pool*)hpool->pool;
		pslice = (elr_mem_slice*)((char*)pool
            - ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));
		if (hpool->tag != pool->slice_tag 
			|| hpool->tag != pslice->tag)
            ret = 0;
    }

    return ret;
}

int _elr_mpl_avail(elr_mem_pool* pool)
{
	elr_mem_slice* pslice = NULL;
	assert(pool != NULL);

	pslice = (elr_mem_slice*)((char*)pool
		- ELR_ALIGN(sizeof(elr_mem_slice), sizeof(int)));

	if (pool->slice_tag != pslice->tag)
		return 0;

	return 1;
}

/*
** Allocate memory from the memory pool。
*/
ELR_MPL_API void*  elr_mpl_alloc(elr_mpl_ht hpool)
{
    elr_mem_slice *pslice = NULL;
    elr_mem_pool  *pool = NULL;
    
    if ( hpool == NULL )
        return NULL;

#ifdef DEBUG
    assert(hpool != NULL  && elr_mpl_avail(hpool)!=0);
#endif

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
	elr_mem_pool  *alloc_pool = NULL;
	int i = 0;

	assert(hpool == NULL || elr_mpl_avail(hpool) != 0);

	if (hpool == NULL)
		hpool = &g_multi_mem_pool;
    
	pool = (elr_mem_pool*)hpool->pool;

	assert(pool->multi != NULL);

	parent_pool = pool->multi[pool->multi_count - 1];

	if(pool->sync == 1)
		pthread_mutex_lock(&pool->pool_mutex);

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
		alloc_mpl = elr_mpl_create(&alloc_mpl, size, parent_pool->on_slice_alloc, parent_pool->on_slice_free);
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

	if (pool->sync == 1)
		pthread_mutex_unlock(&pool->pool_mutex);

	return mem;
}

/*
** Get the size of the memory block requested from the memory pool。
*/
ELR_MPL_API size_t elr_mpl_size(void* mem)
{
    if ( mem == NULL )
        return 0;

    elr_mem_slice *slice = (elr_mem_slice*)((char*)mem
        - ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));
    return slice->node->owner->object_size;
}

/*
** Return memory to the memory pool. Executing this method may also return memory to the system.
*/
ELR_MPL_API void  elr_mpl_free(void* mem)
{
    if ( mem == NULL )
        return;
    
    elr_mem_slice *slice = (elr_mem_slice*)((char*)mem 
        - ELR_ALIGN(sizeof(elr_mem_slice),sizeof(int)));
    elr_mem_node*  node = slice->node;
    elr_mem_pool*  pool = node->owner;

#ifdef DEBUG
	assert(_elr_mpl_avail(pool) != 0);
#endif
	if (pool->sync == 1)
		pthread_mutex_lock(&pool->pool_mutex);
        
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
			if (slice->next != NULL)
				slice->next->prev = slice;
			node->free_slice_tail->next = slice;
			slice->prev = node->free_slice_tail;
			node->free_slice_tail = slice;
        }
    }
    
	if (pool->sync == 1)
    {
        pthread_mutex_unlock(&pool->pool_mutex);
    }

    return;
}

/*
** Destroys the memory pool and its child memory pools.
*/
ELR_MPL_API void elr_mpl_destroy(elr_mpl_ht hpool)
{
    elr_mem_pool  *pool = NULL;
	int            j = 0;

    if ( hpool == NULL )
        return;

#ifdef DEBUG
    assert(hpool!=NULL && elr_mpl_avail(hpool)!=0);
#endif
    pool = (elr_mem_pool*)hpool->pool;
#ifdef DEBUG
    assert(pool->parent != NULL);
#endif
    if ( pool == NULL )
        return;


    if (pool->sync == 1)
        pthread_mutex_lock(&pool->pool_mutex);

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

    hpool->pool = NULL;
    hpool->tag = 0;

    if (pool->sync == 1)
        pthread_mutex_unlock(&pool->pool_mutex);
}

/*
** Terminating the memory pool module will destroy the global memory pool and its sub-memory pools.
** Other memory pools created in the program if there is no explicit release, will also be released after this operation.
*/
ELR_MPL_API void elr_mpl_finalize()
{
    long   refs = 1;
    
    pthread_mutex_lock(&g_mem_pool.pool_mutex);
        
    refs = elr_atomic_dec(&g_mpl_refs);
    if(refs == 0)
    {
        // bug fix, don't lock g_mem_pool.pool_mutex when it going finalize.
        pthread_mutex_unlock(&g_mem_pool.pool_mutex);
        _elr_mpl_destory(&g_mem_pool, 0, 1);
    }
    else
    {
        fprintf( stderr, "refs = elr_atomic_dec(&g_mpl_refs) == %d?\n", refs );
    }
    
    pthread_mutex_unlock(&g_mem_pool.pool_mutex);
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

/* remove an unused NODE, return 0 for no removal */
void _elr_free_mem_node(elr_mem_node* pnode)
{
	assert(pnode->using_slice_count == 0);

	if (pnode->free_slice_head != NULL)
    {
        if(pnode->free_slice_tail->next!=NULL)
            pnode->free_slice_tail->next->prev = pnode->free_slice_head->prev;

        if(pnode->free_slice_head->prev!=NULL)
            pnode->free_slice_head->prev->next = pnode->free_slice_tail->next;

		if (pnode->owner->first_free_slice == pnode->free_slice_head)
                pnode->owner->first_free_slice = pnode->free_slice_tail->next;
        }

	if (pnode->owner->newly_alloc_node == pnode)
		pnode->owner->newly_alloc_node = NULL;

    if(pnode->next != NULL)
        pnode->next->prev = pnode->prev;

    if(pnode->prev != NULL)
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
		pslice->tag++;
        pool->newly_alloc_node->first_avail += pool->slice_size;
        pslice->node = pool->newly_alloc_node;
        
        if(pool->newly_alloc_node->used_slice_count == pool->slice_count)
            pool->newly_alloc_node = NULL;
    }

    return pslice;
}

/*
** Allocate memory from the memory pool.
*/
elr_mem_slice* _elr_slice_from_pool(elr_mem_pool* pool)

{
    elr_mem_slice *slice = NULL;

    if ( pool == NULL )
        return NULL;

#ifdef DEBUG
    assert(pool != NULL);
#endif

    if (pool->sync == 1)
        pthread_mutex_lock(&pool->pool_mutex);

    if(pool->first_free_slice != NULL)
    {
        slice = pool->first_free_slice;
		pool->first_free_slice = slice->next;
		slice->node->free_slice_head = NULL;
        if(pool->first_free_slice != NULL)
		{
            pool->first_free_slice->prev = NULL;
			if(pool->first_free_slice->node == slice->node)
				slice->node->free_slice_head = pool->first_free_slice;
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
    
    if (pool->sync == 1)
        pthread_mutex_unlock(&pool->pool_mutex);

	return slice;
}


void _elr_mpl_destory(elr_mem_pool *pool, int inner, int lock_this)
{
    elr_mem_pool   *temp_pool = NULL;
    elr_mem_node  *temp_node = NULL;
    size_t                  index = 0;

	if (inner == 1 && lock_this == 1 && pool->sync == 1)
        pthread_mutex_lock(&(pool->pool_mutex));
        
	if (inner == 0 && pool->parent != NULL && pool->parent->sync == 1)
		pthread_mutex_lock(&(pool->parent->pool_mutex));

	if (pool->next != NULL)
		pool->next->prev = pool->prev;
    
	if (pool->prev != NULL)
		pool->prev->next = pool->next;

	if (pool->prev == NULL && pool->parent != NULL)
		pool->parent->first_child = pool->next;
    
	if (inner == 0 && pool->parent != NULL && pool->parent->sync == 1)
	    pthread_mutex_unlock(&(pool->parent->pool_mutex));

    while((temp_pool = pool->first_child) != NULL)
    {
		_elr_mpl_destory(temp_pool, 1, lock_this);
	}

	if (pool->sync == 1)
	{
		if (inner == 1 && lock_this == 1)
			pthread_mutex_unlock(&(pool->pool_mutex));
		pthread_mutex_destroy(&pool->pool_mutex);
        pool->sync = 0;
    }

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
    
    if (inner == 1 && lock_this == 1 && pool->sync == 1)
        pthread_mutex_unlock(&pool->pool_mutex);
    
	if(pool != g_multi_mem_pool.pool && pool->multi != NULL)
		elr_mpl_free(pool->multi);
    
	/* free if not the root node */
    if(pool != &g_mem_pool)
    {
        elr_mpl_free(pool);
    }
}
