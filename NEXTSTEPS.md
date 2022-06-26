![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

# Next Steps

The demo exchanges only a simple message with the coap-server. But that is the groundwork for many distributed systems. Not only IoT.

# CoAP / REST 

CoAP follows the [REST](https://de.wikipedia.org/wiki/Representational_State_Transfer) paradigm, also very wellknown by the users of the other representative, HTTP.

That comes with:

- URI ()
- [standardized methods](https://datatracker.ietf.org/doc/html/rfc7252#section-5.8) 
- payload 

and enables to build powerful applications.

Both, the [zephyr client](https://github.com/zephyrproject-rtos/zephyr/tree/main/subsys/net/lib/coap) and the [Eclipse/Californium server](https://github.com/eclipse/californium/tree/main/californium-core#californium-cf---coap-core) supports you to use REST for your application.



