/* See COPYING.txt for license details. */

/*
 * lfrfid.c
 *
 *      Author: thomas
 */

/*************************** I N C L U D E S **********************************/
#include <stdbool.h>

#include "app_freertos.h"
#include "cmsis_os.h"
#include "main.h"
#include "m1_lcd.h"
#include "m1_lp5814.h"
#include "m1_bq27421.h"
#include "m1_power_ctl.h"
#include "m1_bq25896.h"
#include "m1_log_debug.h"
#include "m1_i2c.h"
#include "m1_rf_spi.h"
#include "m1_sdcard.h"
#include "m1_esp32_hal.h"
#include "uiView.h"
#include "privateprofilestring.h"
#include "lfrfid.h"

#define M1_LOGDB_TAG	"RFID"

/*************************** D E F I N E S ************************************/
#define LFRFID_QUEUE_ITEMS_MAX_N		10

#define LFRFID_READ_TIMEOUT_MS   (6000)
#define LFRFID_CARRIER_STABILIZE_MS  50
//************************** C O N S T A N T **********************************/

//************************** S T R U C T U R E S *******************************

/***************************** V A R I A B L E S ******************************/
TaskHandle_t	lfrfid_task_hdl;
TaskHandle_t	lfrfid_rx_task_hdl;
QueueHandle_t	lfrfid_q_hdl;
TimerHandle_t 	lfrfid_read_timeout_handle;
TimerHandle_t	lfrfid_carrier_switch_handle;
volatile lfrfid_carrier_t lfrfid_current_carrier;

LFRFID_TAG_INFO lfrfid_tag_info;
LFRFID_TAG_INFO *lfrfid_tag_info_back;
LFRFIDProgram *lfrfid_program;
lfrfid_state_t lfrfid_state;
uint32_t 	   lfrfid_write_count;

StaticStreamBuffer_t sb_ctrl;
uint8_t sb_storage[LFR_SBUF_BYTES];
StreamBufferHandle_t lfrfid_sb_hdl;
static int lfrfid_lock = 0;
/********************* F U N C T I O N   P R O T O T Y P E S ******************/

static void lfrfidThread(void *param);
static void lfrfid_rxThread(void *param);

/*************** F U N C T I O N   I M P L E M E N T A T I O N ****************/

/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
static void lfrfid_stream_init(void)
{
	lfrfid_sb_hdl = xStreamBufferCreateStatic(
        LFR_SBUF_BYTES,
        LFR_TRIGGER_BYTES,
        sb_storage,
        &sb_ctrl
    );
    configASSERT(lfrfid_sb_hdl);
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
static void lfrfid_stream_deinit(void)
{

	if(lfrfid_sb_hdl)
	{
		vStreamBufferDelete(lfrfid_sb_hdl);
		lfrfid_sb_hdl = NULL;
	}
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
static void lfrfid_read_timeout_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    // TODO:
    m1_app_send_q_message(lfrfid_q_hdl, Q_EVENT_UI_LFRFID_READ_TIMEOUT);
}


/* Bounded post to main_q_hdl. The built-in menu task drains main_q_hdl, but during
 * an app that task is blocked in apps_run and never drains it — an unbounded
 * portMAX_DELAY send here would wedge the RFID engine forever once the queue fills.
 * App reads poll the tag result directly, so a dropped UI event is harmless; the
 * built-in path drains within 100 ms so it never actually drops. */
static void lfrfid_post_main_bounded(S_M1_Q_Event_Type_t evt)
{
    S_M1_Main_Q_t q;
    q.q_evt_type = evt;
    (void)xQueueSend(main_q_hdl, &q, pdMS_TO_TICKS(100));
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void lfrfid_read_timeout_timer_create(void)
{
    if (lfrfid_read_timeout_handle != NULL)
        return;

    lfrfid_read_timeout_handle = xTimerCreate(
        "LFRFID_RD_TO",                    // pcTimerName
        pdMS_TO_TICKS(LFRFID_READ_TIMEOUT_MS), // xTimerPeriodInTicks
        pdFALSE,                           // uxAutoReload: pdFALSE=oneshot
        (void *)0,                         // pvTimerID
        lfrfid_read_timeout_cb             // pxCallbackFunction
    );

    configASSERT(lfrfid_read_timeout_handle != NULL);
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void lfrfid_read_timeout_timer_delete(void)
{
    if (lfrfid_read_timeout_handle == NULL)
        return;

    (void)xTimerStop(lfrfid_read_timeout_handle, 0);

    (void)xTimerDelete(lfrfid_read_timeout_handle, portMAX_DELAY);

    lfrfid_read_timeout_handle = NULL;
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void lfrfid_read_timeout_start(void)
{
    //(void)xTimerStop(lfrfid_read_timeout_handle, 0);
    (void)xTimerChangePeriod(lfrfid_read_timeout_handle,
                             pdMS_TO_TICKS(LFRFID_READ_TIMEOUT_MS), 0);
    //(void)xTimerStart(lfrfid_read_timeout_handle, 0);
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void lfrfid_read_timeout_stop(void)
{
    (void)xTimerStop(lfrfid_read_timeout_handle, 0);
}


/*============================================================================*/
/* Carrier auto-switch timer (ASK <-> PSK)                                    */
/*============================================================================*/
static void lfrfid_carrier_switch_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    m1_app_send_q_message(lfrfid_q_hdl, Q_EVENT_UI_LFRFID_CARRIER_SWITCH);
}

void lfrfid_carrier_switch_timer_create(void)
{
    if (lfrfid_carrier_switch_handle != NULL)
        return;

    lfrfid_carrier_switch_handle = xTimerCreate(
        "LFRFID_CS",
        pdMS_TO_TICKS(LFRFID_CARRIER_SWITCH_MS),
        pdTRUE,                                     /* auto-reload */
        (void *)0,
        lfrfid_carrier_switch_cb
    );
    configASSERT(lfrfid_carrier_switch_handle != NULL);
}

void lfrfid_carrier_switch_timer_delete(void)
{
    if (lfrfid_carrier_switch_handle == NULL)
        return;
    (void)xTimerStop(lfrfid_carrier_switch_handle, 0);
    (void)xTimerDelete(lfrfid_carrier_switch_handle, portMAX_DELAY);
    lfrfid_carrier_switch_handle = NULL;
}

void lfrfid_carrier_switch_timer_start(void)
{
    (void)xTimerChangePeriod(lfrfid_carrier_switch_handle,
                             pdMS_TO_TICKS(LFRFID_CARRIER_SWITCH_MS), 0);
}

void lfrfid_carrier_switch_timer_stop(void)
{
    (void)xTimerStop(lfrfid_carrier_switch_handle, 0);
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void lfrfid_Init(void)
{
	BaseType_t ret;
	size_t free_heap;

	lfrfid_q_hdl = xQueueCreate(LFRFID_QUEUE_ITEMS_MAX_N, sizeof(S_M1_Main_Q_t));
	assert(lfrfid_q_hdl != NULL);

	ret = xTaskCreate(lfrfidThread, "lfrfid_task_n", M1_TASK_STACK_SIZE_1024, NULL, TASK_PRIORITY_SYS_INIT, &lfrfid_task_hdl);
	assert(ret==pdPASS);
	assert(lfrfid_task_hdl!=NULL);
	free_heap = xPortGetFreeHeapSize();
	assert(free_heap >= M1_LOW_FREE_HEAP_WARNING_SIZE);

	lfrfid_stream_init();

	ret = xTaskCreate(lfrfid_rxThread, "lfrfid_rx_task_n", M1_TASK_STACK_SIZE_4096, NULL, TASK_PRIORITY_SYS_INIT, &lfrfid_rx_task_hdl);
	assert(ret==pdPASS);
	assert(lfrfid_rx_task_hdl!=NULL);
	free_heap = xPortGetFreeHeapSize();
	assert(free_heap >= M1_LOW_FREE_HEAP_WARNING_SIZE);

	lfrfid_read_timeout_timer_create();
	lfrfid_carrier_switch_timer_create();
	lfrfid_current_carrier = LFRFID_CARRIER_ASK;

	set_line_buffer_size(512);

	lfrfid_encoded_data.data = malloc(sizeof(Encoded_Data_t)*ENCODED_DATA_MAX);
	assert(lfrfid_encoded_data.data!=NULL);
	free_heap = xPortGetFreeHeapSize();
	assert(free_heap >= M1_LOW_FREE_HEAP_WARNING_SIZE);

	lfrfid_program = malloc(sizeof(LFRFIDProgram));
	assert(lfrfid_program!=NULL);
	free_heap = xPortGetFreeHeapSize();
	assert(free_heap >= M1_LOW_FREE_HEAP_WARNING_SIZE);

	lfrfid_tag_info_back = malloc(sizeof(LFRFID_TAG_INFO));
	assert(lfrfid_tag_info_back!=NULL);
	free_heap = xPortGetFreeHeapSize();
	assert(free_heap >= M1_LOW_FREE_HEAP_WARNING_SIZE);
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void lfrfid_DeInit(void)
{
	/* Delete the timers FIRST — before the queue/tasks. Both timer callbacks post
	 * into lfrfid_q_hdl; the carrier-switch timer is AUTO-RELOAD. Deleting the queue
	 * first (as this used to) left a window where a timer fire posted into the
	 * freed/NULL queue via an unconditional portMAX_DELAY send -> HardFault reboot.
	 * xTimerDelete() blocks until the daemon processes it, so once both are gone no
	 * callback can run again; only then is it safe to delete the queue. */
	lfrfid_read_timeout_timer_delete();
	lfrfid_carrier_switch_timer_delete();

	if(lfrfid_task_hdl)
	{
		vTaskDelete(lfrfid_task_hdl);
		lfrfid_task_hdl = NULL;
	}

	if(lfrfid_q_hdl)
	{
		vQueueDelete(lfrfid_q_hdl);
		lfrfid_q_hdl = NULL;
	}

	if(lfrfid_rx_task_hdl)
	{
		vTaskDelete(lfrfid_rx_task_hdl);
		lfrfid_rx_task_hdl = NULL;
	}

	lfrfid_stream_deinit();

	safe_free((void**)&lfrfid_encoded_data.data);
	safe_free((void**)&lfrfid_program);
	safe_free((void**)&lfrfid_tag_info_back);
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void task_suspend_safe(TaskHandle_t xHandle)
{
#if 1
	TaskStatus_t xTaskDetails;

	vTaskGetInfo(xHandle, &xTaskDetails, pdTRUE, eInvalid);
	if (xTaskDetails.eCurrentState != eSuspended) {
		vTaskSuspend(xHandle);
	}
#endif
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void task_resume_safe(TaskHandle_t xHandle)
{
#if 1
	TaskStatus_t xTaskDetails;

	vTaskGetInfo(xHandle, &xTaskDetails, pdTRUE, eInvalid);
	if (xTaskDetails.eCurrentState == eSuspended) {
		vTaskResume(xHandle);
	}
#endif
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
inline void safe_free(void **pp)
{
    if (pp && *pp) {
        free(*pp);
        *pp = NULL;
    }
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
static void bytes_to_u32_be(const uint8_t in_data[], uint32_t out_data[], int size)
{
    for (int i = 0; i < size; i++) {
    	out_data[i] =
            ((uint32_t)in_data[i * 4 + 0] << 24) |
            ((uint32_t)in_data[i * 4 + 1] << 16) |
            ((uint32_t)in_data[i * 4 + 2] <<  8) |
            ((uint32_t)in_data[i * 4 + 3] <<  0);
    }
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
static void bytes_to_u32_le(const uint8_t in_data[], uint32_t out_data[], int size)
{
    for (int i = 0; i < size; i++) {
    	out_data[i] =
            ((uint32_t)in_data[i * 4 + 0] <<  0) |
            ((uint32_t)in_data[i * 4 + 1] <<  8) |
            ((uint32_t)in_data[i * 4 + 2] << 16) |
            ((uint32_t)in_data[i * 4 + 3] << 24);
    }
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void bytes_to_u32_array(BitOrder order, const uint8_t in_data[], uint32_t out_data[], int size)
{
	if(order == BIT_ORDER_LSB_FIRST)
		bytes_to_u32_le(in_data,out_data,size);
	else
		bytes_to_u32_be(in_data,out_data,size);
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
//lfrfid_evt_t sample_data[120];
void lfrfid_rxThread(void *param)
{
	uint8_t  batch_buf[LFR_TRIGGER_BYTES];
	memset(batch_buf,0,sizeof(batch_buf));

	lfrfid_decoder_begin();
	while(1)
	{
        size_t n = xStreamBufferReceive(lfrfid_sb_hdl, batch_buf, sizeof(batch_buf), LFR_RX_TIMEOUT);

        if(n == 0 || lfrfid_lock == 0){
            continue;
        }

        /* Determine which feature set to try based on current carrier.
         * Both ASK 125 kHz and ASK 134.2 kHz use ASK decoders — the
         * FDX-B timing windows (68-188 / 196-316 µs) cover both frequencies. */
        uint32_t active_feature = (lfrfid_current_carrier == LFRFID_CARRIER_PSK)
                                  ? LFRFIDFeaturePSK : LFRFIDFeatureASK;

        uint16_t total_events = n / LFR_ITEM_SIZE;
        uint8_t CHUNK_SIZE = FRAME_CHUNK_SIZE>>1;
        lfrfid_evt_t* p = (lfrfid_evt_t*)batch_buf;

        for (int i = 0; i < total_events; i += CHUNK_SIZE)
        {
            uint16_t events_to_process = (total_events - i < CHUNK_SIZE) ? (total_events - i) : CHUNK_SIZE;

            for(int protoIdx=LFRFIDProtocolEM4100; protoIdx<LFRFIDProtocolMax; protoIdx++)
            {
                /* Only run decoders that match the current carrier mode */
                if(lfrfid_protocols[protoIdx] &&
                   (lfrfid_protocols[protoIdx]->features & active_feature))
                {
                    if(lfrfid_decoder_execute(protoIdx, &p[i], events_to_process))
                    {
                        lfrfid_tag_info.protocol = protoIdx;
                        m1_app_send_q_message(lfrfid_q_hdl, Q_EVENT_LFRFID_TAG_DETECTED);
                        break;  /* first match wins — stop checking other protocols */
                    }
                }
            }
        }

	}
}


/*============================================================================*/
/**
  * @brief
  * @param
  * @retval
  */
/*============================================================================*/
void lfrfidThread(void *param)
{
	S_M1_Main_Q_t q_item;
	BaseType_t ret;

	while(1)
	{
		ret = xQueueReceive(lfrfid_q_hdl, &q_item, portMAX_DELAY);
		if (ret==pdTRUE)
		{
			if ( q_item.q_evt_type==Q_EVENT_LFRFID_TAG_DETECTED )
			{
				lfrfid_lock = 0;

				lfrfid_read_timeout_stop();
				lfrfid_carrier_switch_timer_stop();

				lfrfid_read_hw_deinit();
				osDelay(10);

				task_suspend_safe(lfrfid_rx_task_hdl);

				lfrfid_post_main_bounded(Q_EVENT_LFRFID_TAG_DETECTED);
			} // else if ( q_item.q_evt_type==Q_EVENT_LFRFID_TAG_DETECTED )
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_START_READ)
			{
				if(lfrfid_lock)
					continue;

				task_resume_safe(lfrfid_rx_task_hdl);

				lfrfid_isr_init();
				lfrfid_current_carrier = LFRFID_CARRIER_ASK;
				lfrfid_read_hw_init();
				lfrfid_decoder_begin();
				lfrfid_tag_info_init();

				lfrfid_read_timeout_start();
				lfrfid_carrier_switch_timer_start();

				lfrfid_state = LFRFID_STATE_READ;
				lfrfid_lock = 1;
				// RFID_PULL output LOW (MUTE OFF - DET)
				HAL_GPIO_WritePin(RFID_PULL_GPIO_Port, RFID_PULL_Pin, GPIO_PIN_RESET);
				//READ_STEP = RFIN_PREAMBLE;
				osDelay(50);
			}
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_CARRIER_SWITCH)
			{
				if(lfrfid_lock && lfrfid_state == LFRFID_STATE_READ)
				{
					/* Cycle carrier: ASK 125k -> ASK 134.2k -> PSK -> ASK 125k ...
					 * ASK 134.2 kHz supports FDX-B pet/animal chips (ISO 11784/11785) */
					if(lfrfid_current_carrier == LFRFID_CARRIER_ASK)
					{
						lfrfid_current_carrier = LFRFID_CARRIER_ASK_134;
						lfrfid_carrier_switch(LFRFID_CARRIER_ASK_134_FREQ,
						                      LFRFID_CARRIER_ASK_134_DUTY);
					}
					else if(lfrfid_current_carrier == LFRFID_CARRIER_ASK_134)
					{
						lfrfid_current_carrier = LFRFID_CARRIER_PSK;
						lfrfid_carrier_switch(LFRFID_CARRIER_PSK_FREQ,
						                      LFRFID_CARRIER_PSK_DUTY);
					}
					else
					{
						lfrfid_current_carrier = LFRFID_CARRIER_ASK;
						lfrfid_carrier_switch(LFRFID_CARRIER_ASK_FREQ,
						                      LFRFID_CARRIER_ASK_DUTY);
					}
					/* Reset decoders after carrier switch */
					lfrfid_isr_init();
					lfrfid_decoder_begin();
					/* Brief stabilization delay */
					osDelay(LFRFID_CARRIER_STABILIZE_MS);
				}
			}
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_STOP)
			{
				/* Always tear the read down — do NOT gate on
				 * rfid_rxtx_is_taking_this_irq, which is never set for a read. A
				 * no-tag read (timeout or cancel) otherwise left lfrfid_lock=1 and
				 * the auto-reload carrier timer running: the next START_READ was
				 * refused (lock stuck) and DeInit could fire the timer into the
				 * freed queue. Idempotent when TAG_DETECTED already cleaned up. */
				lfrfid_lock = 0;
				lfrfid_state = LFRFID_STATE_IDLE;

				lfrfid_read_timeout_stop();
				lfrfid_carrier_switch_timer_stop();
				lfrfid_read_hw_deinit();
				osDelay(10);

				task_suspend_safe(lfrfid_rx_task_hdl);
				rfid_rxtx_is_taking_this_irq = 0;
			}
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_EMULATE)
			{
				lfrfid_state = LFRFID_STATE_EMULATE;
				lfrfid_encoder_begin(lfrfid_tag_info.protocol,&lfrfid_tag_info);
				lfrfid_encoder_send(lfrfid_tag_info.protocol,NULL);

			    rfid_rxtx_is_taking_this_irq = 1;

			}
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_EMULATE_STOP)
			{
				lfrfid_state = LFRFID_STATE_IDLE;
				lfrfid_emul_hw_deinit();

			}
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_WRITE)
			{
				if(lfrfid_write_count)
				{
					lfrfid_lock = 0;
					lfrfid_state = LFRFID_STATE_IDLE;

					rfid_rxtx_is_taking_this_irq = 1; // dummy
					lfrfid_read_timeout_stop();
					lfrfid_read_hw_deinit();
					osDelay(10);

					task_suspend_safe(lfrfid_rx_task_hdl);
					HAL_GPIO_WritePin(RFID_PULL_GPIO_Port, RFID_PULL_Pin, GPIO_PIN_SET);
					osDelay(52);
				}

				lfrfid_write_count++;

				lfrfid_state = LFRFID_STATE_WRITE;
				lfrfid_program->type = LFRFIDProgramTypeT5577;
				lfrfid_write_begin(lfrfid_tag_info.protocol,&lfrfid_tag_info, lfrfid_program);
				lfrfid_write_send(lfrfid_tag_info.protocol, NULL);

				osDelay(1000);
#if 0
				m1_app_send_q_message(lfrfid_q_hdl, Q_EVENT_UI_LFRFID_WRITE_DONE);
			}
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_WRITE_DONE)
			{
#endif
				lfrfid_lock = 0;
				lfrfid_post_main_bounded(Q_EVENT_UI_LFRFID_WRITE_DONE);
			}
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_READ_TIMEOUT)
			{
				//lfrfid_lock = 0;
				lfrfid_post_main_bounded(Q_EVENT_UI_LFRFID_READ_TIMEOUT);
			}
			else if(q_item.q_evt_type==Q_EVENT_UI_LFRFID_WRITE_STOP)
			{
				lfrfid_lock = 0;
				lfrfid_state = LFRFID_STATE_IDLE;

				rfid_rxtx_is_taking_this_irq = 1; // dummy
				lfrfid_read_timeout_stop();
				lfrfid_read_hw_deinit();
				osDelay(10);
				task_suspend_safe(lfrfid_rx_task_hdl);
			}

		} // if (ret==pdTRUE)
	}
}
