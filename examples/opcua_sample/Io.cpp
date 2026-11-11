#include <tx_api.h>

int main()
{
    // HAL startup establishes inactive STEP/ENABLE levels. No motor objects,
    // ICC endpoints or motion workers are constructed by this sample image.
    while (true) {
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
}
