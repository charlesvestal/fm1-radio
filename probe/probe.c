/* The link probe: start the BLE stack the way the SDK's demo does, as a peripheral
 * (apps/common/ble/le_trans_data.c), and nothing else. Linked without JieLi's OS. */
extern void btstack_init(void);
extern void bt_ble_init(void);
void probe_entry(void)
{
    btstack_init();
    bt_ble_init();
}
