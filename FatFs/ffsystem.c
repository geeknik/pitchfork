/*------------------------------------------------------------------------*/
/* A Sample Code of User Provided OS Dependent Functions for FatFs        */
/*------------------------------------------------------------------------*/

#include "ff.h"

#if FF_FS_REENTRANT
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* FatFs requests one mutex per volume (plus an optional system mutex). The
 * previous sample implementation returned success without taking anything,
 * leaving SD reads from the menu and qM's RPC worker free to overlap. Static
 * storage keeps this available during early boot as well as after scheduling. */
#define FF_MUTEX_COUNT  (FF_VOLUMES + 1)
static StaticSemaphore_t s_ff_mutex_storage[FF_MUTEX_COUNT];
static SemaphoreHandle_t s_ff_mutex[FF_MUTEX_COUNT];
#endif


#if FF_USE_LFN == 3	/* Use dynamic memory allocation */

/*------------------------------------------------------------------------*/
/* Allocate/Free a Memory Block                                           */
/*------------------------------------------------------------------------*/


void* ff_memalloc (	/* Returns pointer to the allocated memory block (null if not enough core) */
	UINT msize		/* Number of bytes to allocate */
)
{
	void* mem_ptr = NULL; /*  Memory block to be allocated */
	
	return mem_ptr;
}


void ff_memfree (
	void* mblock	/* Pointer to the memory block to free (no effect if null) */
)
{
	free(mblock);	/* Free the memory block */
}

#endif




#if FF_FS_REENTRANT	/* Mutal exclusion */
/*------------------------------------------------------------------------*/
/* Create a Mutex                                                         */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount function to create a new mutex
/  or semaphore for the volume. When a 0 is returned, the f_mount function
/  fails with FR_INT_ERR.
*/

int ff_mutex_create (	/* Returns 1:Function succeeded or 0:Could not create the mutex */
	int vol				/* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
  if (vol < 0 || vol >= FF_MUTEX_COUNT)
    return 0;
  if (s_ff_mutex[vol] == NULL)
    s_ff_mutex[vol] = xSemaphoreCreateMutexStatic(&s_ff_mutex_storage[vol]);
  return s_ff_mutex[vol] != NULL;
}


/*------------------------------------------------------------------------*/
/* Delete a Mutex                                                         */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount function to delete a mutex or
/  semaphore of the volume created with ff_mutex_create function.
*/

void ff_mutex_delete (	/* Returns 1:Function succeeded or 0:Could not delete due to an error */
	int vol				/* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
  /* Static FreeRTOS mutex storage lives for the firmware lifetime. FatFs calls
   * this only while unmounting, and the next mount reuses the same lock. */
  (void)vol;
}


/*------------------------------------------------------------------------*/
/* Request a Grant to Access the Volume                                   */
/*------------------------------------------------------------------------*/
/* This function is called on enter file functions to lock the volume.
/  When a 0 is returned, the file function fails with FR_TIMEOUT.
*/

int ff_mutex_take (	/* Returns 1:Succeeded or 0:Timeout */
	int vol			/* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
  if (vol < 0 || vol >= FF_MUTEX_COUNT || s_ff_mutex[vol] == NULL)
    return 0;
  /* Early boot is single-threaded. A timed FreeRTOS wait is invalid before the
   * scheduler starts, so leave the already-created mutex untouched there. */
  if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    return 1;
  return xSemaphoreTake(s_ff_mutex[vol], FF_FS_TIMEOUT) == pdTRUE;
}



/*------------------------------------------------------------------------*/
/* Release a Grant to Access the Volume                                   */
/*------------------------------------------------------------------------*/
/* This function is called on leave file functions to unlock the volume.
*/

void ff_mutex_give (
	int vol			/* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
  if (vol < 0 || vol >= FF_MUTEX_COUNT || s_ff_mutex[vol] == NULL)
    return;
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    xSemaphoreGive(s_ff_mutex[vol]);
}

#endif	/* FF_FS_REENTRANT */

