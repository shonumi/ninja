#include <stdio.h>
#include <stdlib.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <unistd.h>

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

vu32 com_err = 0;

vu32* const si_reg = (u32*)0xCD006400;
vu32* const si_buf = (u32*)0xCD006480;

u32 dump_count = 0;

bool mode_32 = true;

void save_file_32(u32* save_data, u32 save_size)
{
	//Create output binary
	FILE* out_file;

	if(!dump_count) { out_file = fopen("joy_dump.bin", "w+"); }
	else { out_file = fopen("joy_dump.bin", "a"); }

	if(out_file == NULL) { printf("libfat ERROR!\n"); return; }
	
	fwrite(save_data, 4, save_size, out_file);
	fclose(out_file);

	dump_count++;
}

void save_file_16(u16* save_data, u32 save_size)
{
	//Create output binary
	FILE* out_file;

	if(!dump_count) { out_file = fopen("joy_dump.bin", "w+"); }
	else { out_file = fopen("joy_dump.bin", "a"); }

	if(out_file == NULL) { printf("libfat ERROR!\n"); return; }
	
	fwrite(save_data, 2, save_size, out_file);
	fclose(out_file);

	dump_count++;
}

u32 swap_bytes(u32 input)
{
	u32 result = 0;

	result |= ((input & 0xFF000000) >> 24);
	result |= ((input & 0x00FF0000) >> 8);
	result |= ((input & 0x0000FF00) << 8);
	result |= ((input & 0x000000FF) << 24);

	return result;
}

u32 swap_bytes_half(u32 input)
{
	u32 result = 0;
	
	result |= ((input & 0xFFFF0000) >> 16);
	result |= ((input & 0x0000FFFF) << 16);

	return result;
}

//Generates 32-bit value that can be written to SICOMCSR to start a transfer on a given channel
u32 generate_com_csr(u32 channel, u32 in_len, u32 out_len)
{
	u32 com_csr = 0;

	in_len &= 0x7F;
	out_len &= 0x7F;
	channel &= 0x3;
		
	//Channel
	com_csr |= (channel << 25);

	//Channel Enable
	com_csr |= (1 << 24);

	//Output Length
	com_csr |= (out_len << 16);

	//Input Length
	com_csr |= (in_len << 8);

	//Command Enable
	com_csr |= (1 << 7);

	//Channel 2?
	com_csr |= (channel << 1);

	//Callback Enable
	com_csr |= (0 << 6);

	//TSTART
	com_csr |= 1;

	si_reg[14] = 0x20202020;

	return com_csr;
}

//Sends the JOYBUS command 0x00 to the given channel
//Used to find out the device ID (written to SI buffer)
void ping_ID(u32 channel)
{
	//Clear SI buffer
	for(int x = 0; x < 0x20; x++) { si_buf[x] = 0; }
	
	//Setup JOYBUS command 0x00
	si_buf[0] = 0x00;

	//Write to SICOMCSR to start transfer, wait for any pending transfers first
	while(si_reg[13] & 0x1) { }
	si_reg[13] = generate_com_csr(channel, 3, 1);

	//Wait for next SICOMCSR transfer to finish
	while(si_reg[13] & 0x1) { }	
}

//Sends 0x14 command to GBA
void send_14_cmd(u32 channel)
{
	//Clear SI buffer
	for(int x = 0; x < 0x80; x++) { si_buf[x] = 0; }

	//Set up JOYBUS command
	si_buf[0] = 0x14000000;

	//Clear SISR Error Bits
	si_reg[14] |= (0x0F << ((3 - channel) * 8));

	//Write to SICOMCSR to start transfer, wait for any pending transfers first
	while(si_reg[13] & 0x1) { }
	si_reg[13] = generate_com_csr(channel, 4, 1);

	//Wait for next SICOMCSR transfer to finish
	while(si_reg[13] & 0x1) { }
}

int main(int argc, char **argv)
{
	// Initialise the video system
	VIDEO_Init();

	// This function initialises the attached controllers
	WPAD_Init();

	// Obtain the preferred video mode from the system
	// This will correspond to the settings in the Wii menu
	rmode = VIDEO_GetPreferredMode(NULL);

	// Allocate memory for the display in the uncached region
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	// Initialise the console, required for printf
	console_init(xfb,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);

	// Set up the video registers with the chosen mode
	VIDEO_Configure(rmode);

	// Tell the video hardware where our display memory is
	VIDEO_SetNextFramebuffer(xfb);

	// Make the display visible
	VIDEO_SetBlack(FALSE);

	// Flush the video register changes to the hardware
	VIDEO_Flush();

	// Wait for Video setup to complete
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();

	//Init FAT
	fatInitDefault();

	u32 vcount = 0;
	u32 g_state = 0;

	u32 capture_length = 0;
	u32 capture_count = 0;

	u32 save_buf_32[0x200];
	u16 save_buf_16[0x200];

	for(u32 x = 0; x < 0x200; x++)
	{
		save_buf_32[x] = 0;
		save_buf_16[x] = 0;
	}

	printf("\n\n\nInsert GBA...\n");

	while(1) {
		vcount++;

		if(vcount == 1)
		{
			vcount = 0;

			//Detect Game Boy Advance
			if(g_state == 0)
			{
				ping_ID(1);
				u32 joybus_id = (si_buf[0] >> 16);

				if(joybus_id == 0x0004)
				{
					g_state = 1;
					printf("GBA Detected!\n");
					printf("Entering Data Capture Mode...\n");
				}

				else if(joybus_id != 0)
				{
					printf("Other Device Detected -> 0x%x\n", joybus_id);
				}
			}

			//Grab length of transmission
			else if(g_state == 1)
			{
				send_14_cmd(1);
				capture_length = swap_bytes(si_buf[0]);
					
				if((si_reg[14] & 0xF) == 0)
				{
					if((capture_length > 0) && (capture_length <= 0x200))
					{
						if(mode_32)
						{
							printf("Incoming Data: %d bytes total ... ", (capture_length * 4));
						}

						else
						{
							printf("Incoming Data: %d bytes total ... ", (capture_length * 2));
						}

						g_state = 2;
						capture_count = 0;
					}
				}
			}

			//Capture data from GBA
			else if(g_state == 2)
			{
				while(g_state == 2)
				{
					send_14_cmd(1);

					if(mode_32) { save_buf_32[capture_count++] = si_buf[0]; }
					else { save_buf_16[capture_count++] = swap_bytes_half(si_buf[0]); }

					if(capture_count >= capture_length)
					{
						g_state = 1;
						printf("Capture complete!\n");

						//Save data to file
						if(mode_32) { save_file_32(save_buf_32, capture_length); }
						else { save_file_16(save_buf_16, capture_length); }
					}

					usleep(1000);
				}
			}
		}		

		// Call WPAD_ScanPads each loop, this reads the latest controller states
		WPAD_ScanPads();

		// WPAD_ButtonsDown tells us which buttons were pressed in this loop
		// this is a "one shot" state which will not fire again until the button has been released
		u32 pressed = WPAD_ButtonsDown(0);

		// We return to the launcher application via exit
		if(pressed & WPAD_BUTTON_HOME)
		{
			printf("Exiting...\n");
			exit(0);
		}

		//Switch between 32-bit and 16-bit modes
		if(pressed & WPAD_BUTTON_PLUS)
		{
			if(mode_32)
			{
				printf("Switching to 16-bit mode\n");
				mode_32 = false;
			}

			else
			{
				printf("Switching to 32-bit mode\n");
				mode_32 = true;
			}
		}

		// Wait for the next frame
		VIDEO_WaitVSync();
	}

	return 0;
}
