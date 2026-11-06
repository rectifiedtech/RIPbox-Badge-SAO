# Supercon v1.0: Known Hardware Issues

These are the boards given out at **Hackaday Supercon 2026**. They have **three known errors**, all corrected in [v1.1](../v1.1).

The stock firmware ([`firmware/supercon-v1.0`](../../firmware/supercon-v1.0)) works on these boards as-is. The reworks below only affect the SAO connector and the logo LED.

---

## 1. 3.3 V is connected to the SDA and SCL pins

**What's wrong:** On the SAO connector, the 3.3 V supply was routed to the **SDA and SCL pins** instead of the **far-left pins**, where power is supposed to be.

**Effect:** The connector doesn't provide power in the places an SAO expects it, and SDA/SCL can't be used as I²C on the connector.

**Workaround:** **Bridge the SDA and SCL pins over to the far-left (power) pins** with a short jumper wire or a solder bridge to ensure power delivery, please refer to the schematic for the SAO connector errors in detail.

*Rework photo: coming soon.*

**Fixed in v1.1:** yes.

---

## 2. SAO GPIO pins are not connected to GPIO 1 and 2

**What's wrong:** Because of a schematic error, the SAO connector's GPIO pins are **not connected to ESP8266 GPIO 1 and GPIO 2**.

**Effect:** The SAO GPIO1 and GPIO2 pins don't do anything. The stock firmware doesn't use them, so normal badge operation isn't affected.

**Workaround:** If you want them, run bodge wires from the connector's GPIO pads to the ESP8266 pins you want to use.

**Fixed in v1.1:** yes.

---

## 3. Logo LED polarity is backwards

**What's wrong:** The logo LED footprint/symbol is wired the wrong way around.

**Effect:** The LED won't light, so the logo doesn't glow and the flashing alert for pings/messages is invisible. (The on-screen alerts still work.)

**Workaround:** **Desolder the logo LED and re-solder it rotated 180° and flipped over in the upfiring orientation.** After that, the firmware drives it as intended: on by default, flashing during alerts.

**Fixed in v1.1:** yes.

---

## Which board do I have?

If you received your RIPbox at Supercon 2026, it's **Supercon v1.0**.
