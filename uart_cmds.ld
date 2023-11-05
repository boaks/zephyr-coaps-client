/* AT monitors */
. = ALIGN(4);
_uart_cmd_entry_list_start = .;
KEEP(*(SORT_BY_NAME("._uart_cmd_entry.*")));
_uart_cmd_entry_list_end = .;
