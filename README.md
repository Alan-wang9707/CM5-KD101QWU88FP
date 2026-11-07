# Hardware 
KD101BWX49HP-DC88\
How to use cm4 driver a 10.1 inch 800*1280 display and touch panel（ILI9881C+GT928)\
We provide industrial tft lcd and touch panel, and made a convert board to connect our panel to CM4/CM5 directly.\
<img width="425" height="58" alt="image" src="https://github.com/user-attachments/assets/677e2c1c-a3a4-4136-8d96-9307796879a9" />\
(https://www.kadidisplay.com/products/10-1-ips-8001280-mipi-dsi-and-i2c-touch-for-raspberry-pi%ef%bc%88cm4-cm5%ef%bc%89/)
# Software
# Step 1：
· Use the official Raspberry Pi image burning tool, Raspberry Pi Imager, to download the latest version image to the SD card.\
· And download VMware virtual machine and configure the Ubuntu LTS version image as a cross-compiler.\
<img width="125" height="151" alt="image" src="https://github.com/user-attachments/assets/033d8e9b-b6dc-4235-aa8d-42f7873e998a" />\
# Step 2：
· Pull the latest Raspberry Pi source code in a virtual machine. \
<img width="627" height="79" alt="image" src="https://github.com/user-attachments/assets/8dab5eae-6d12-4cfa-a586-497193d5bb1f" /> \
· And git clone our driver and device tree and place them in the corresponding directory \
<img width="761" height="44" alt="image" src="https://github.com/user-attachments/assets/b9a593f7-ea41-482d-b3ed-74e27fd0387f" />
# Step 3：
· Compile the kernel and device tree according to the official procedure to generate the corresponding .KO and .dtbo files.\
详情请见：https://www.raspberrypi.com/documentation/computers/linux_kernel.html#kernel
# Step 4：
· Transfer the compiled .ko and .dtbo files to CM4 via SSH or by copying them directly to an SD card.
# Step 5：
· Place the .dtbo file in the /boot/firmware/overlays folder. \
· Place the .ko file in the /lib/modules/$(uname -r)/extra/ folder. \
· sudo depmod -a
#  Step 6：
· Simply modify your config.txt file and add the corresponding dtoverlay.\
<img width="581" height="78" alt="image" src="https://github.com/user-attachments/assets/c7ce6748-dd2b-4f21-a166-2d3a51a7e159" />

#  PWM_Backlight :
<img width="532" height="648" alt="image" src="https://github.com/user-attachments/assets/974c311a-633c-4413-8560-1573f5100479" />

# Note:
Kadi Display is a solutions-oriented display provider. We offer in-house production and design of LCD and touch panels, along with Raspberry Pi-ready display drivers, enabling developers and customers to quickly deploy and implement their applications.
# Contact：sales@sz-kadi.com|Whatsapp：+86-13662585086
