#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <libgen.h>
#include <errno.h>
#include <sys/stat.h>

#include "app.h"
#include "frametimer.h"
#include "crtemu.h"
#include "crtemu_pc.h"
#include "crt_frame.h"
#include "crt_frame_pc.h"


#define GB_INI_IMPLEMENTATION
#include "gb_ini.h"


#include "shared.h"

#include "smsplus.h"

static settings_t settings;
static gamedata_t gdata;


static uint8_t *pixels = NULL;

void BGR_to_RGB(int size)
{
APP_U32 *p = (APP_U32*)&pixels[0];
	for (int y=0;y<size;y++)
	{
		APP_U32 inp = *p;
		*p++=((inp&0xff0000)>>16) | (inp&0xff00)  | ((inp&0xff)<<16);
	}
}


void smsp_state(int slot, int mode) {
	// Save and Load States
	char stpath[PATH_MAX];
	snprintf(stpath, sizeof(stpath), "%s%s.st%d", gdata.stdir, gdata.gamename, slot);
	
	FILE *fd;
	
	switch(mode) {
		case 0:
			fd = fopen(stpath, "wb");
			if (fd) {
				system_save_state(fd);
				fclose(fd);
			}
			break;
		
		case 1:
			fd = fopen(stpath, "rb");
			if (fd) {
				system_load_state(fd);
				fclose(fd);
			}
			break;
	}
}

void system_manage_sram(uint8_t *sram, int slot, int mode) {
	// Set up save file name
	FILE *fd;
	
	switch(mode) {
		case SRAM_SAVE:
			if(sms.save) {
				fd = fopen(gdata.sramfile, "wb");
				if (fd) {
					fwrite(sram, 0x8000, 1, fd);
					fclose(fd);
				}
			}
			break;
		
		case SRAM_LOAD:
			fd = fopen(gdata.sramfile, "rb");
			if (fd) {
				sms.save = 1;
				fread(sram, 0x8000, 1, fd);
				fclose(fd);
			}
			else { memset(sram, 0x00, 0x8000); }
			break;
	}
}

static GB_INI_HANDLER(smsp_ini_handler) {
//const *data, char const *section, char const *name, char const *value
	#define TEST(s, n) (strcmp(section, s) == 0 && strcmp(name, n) == 0)
	if (TEST("video", "scale")) { settings.video_scale = atoi(value); }
	else if (TEST("video", "filter")) { settings.video_filter = atoi(value); }
	else if (TEST("audio", "rate")) { settings.audio_rate = atoi(value); }
	else if (TEST("audio", "fm")) { settings.audio_fm = atoi(value); }
	else if (TEST("audio", "fmtype")) { settings.audio_fmtype = atoi(value); }
	else if (TEST("misc", "region")) { settings.misc_region = atoi(value); }
	else if (TEST("misc", "ffspeed")) { settings.misc_ffspeed = atoi(value); }
	else { return 0; }
	#undef TEST
	return 1;
}

static void smsp_gamedata_set(char *filename) {
	// Set paths, create directories
	
	// Set the game name
	snprintf(gdata.gamename, sizeof(gdata.gamename), "%s", basename(filename));
	
	// Strip the file extension off
	for (int i = strlen(gdata.gamename) - 1; i > 0; i--) {
		if (gdata.gamename[i] == '.') {
			gdata.gamename[i] = '\0';
			break;
		}
	}
	
	// Set up the sram directory
	snprintf(gdata.sramdir, sizeof(gdata.sramdir), "sram/");
#ifdef _MINGW
	if (mkdir(gdata.sramdir) && errno != EEXIST) {
#else
	if (mkdir(gdata.sramdir, 0755) && errno != EEXIST) {
#endif
		fprintf(stderr, "Failed to create %s: %d\n", gdata.sramdir, errno);
	}
	
	// Set up the sram file
	snprintf(gdata.sramfile, sizeof(gdata.sramfile), "%s%s.sav", gdata.sramdir, gdata.gamename);
	
	// Set up the state directory
	snprintf(gdata.stdir, sizeof(gdata.stdir), "state/");
#ifdef _MINGW
	if (mkdir(gdata.stdir) && errno != EEXIST) {
#else
	if (mkdir(gdata.stdir, 0755) && errno != EEXIST) {
#endif
		fprintf(stderr, "Failed to create %s: %d\n", gdata.stdir, errno);
	}
	
	// Set up the screenshot directory
#ifdef _MINGW
	if (mkdir("screenshots/") && errno != EEXIST) {
#else
	if (mkdir("screenshots/", 0755) && errno != EEXIST) {
#endif
		fprintf(stderr, "Failed to create %s: %d\n", "screenshots/", errno);
	}
}


#define VIDEO_WIDTH_SMS 256
#define VIDEO_HEIGHT_SMS 192
#define VIDEO_WIDTH_GG 160
#define VIDEO_HEIGHT_GG 144

void smsp_video_create_buffer() {
	// Create video buffer
	pixels = calloc(VIDEO_WIDTH_SMS * VIDEO_HEIGHT_SMS * 4, 1);
}

uint8_t *smsp_video_pixels_ptr() { return pixels; }

typedef struct aq_t {
	uint32_t front; // Front of the queue
	uint32_t rear; // Rear of the queue
	uint32_t qsize; // Size of the queue
	uint32_t bsize; // Size of the buffer
	int16_t *buffer; // Pointer to the buffer
} aq_t;

static aq_t aq = {0};

#define BUFSIZE 6400
#define CHANNELS 2

#define USE_MINI_AUDIO

#ifdef USE_MINI_AUDIO

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_PULSEAUDIO
#define MA_NO_JACK
#define MA_NO_AAUDIO
#define MA_NO_OPENSL
#define MA_NO_WEBAUDIO
#define MA_NO_DECODING
#define MA_NO_STDIO
#include "miniaudio.h"


static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static ma_device madevice;
static int16_t audiobuf[BUFSIZE];


static inline void aq_enq(int16_t *data, size_t size) {
	// Don't overflow the buffer
	while (aq.qsize >= aq.bsize - (size + 1)) {
		//fprintf(stderr, "Audio Queue full!\n");
	}
	// Lock before adding new data
	pthread_mutex_lock(&mutex);
	for (int i = 0; i < size; i++) { // Populate the queue
		aq.buffer[aq.rear] = data[i];
		aq.rear = (aq.rear + 1) % aq.bsize;
		aq.qsize++;
	}
	pthread_mutex_unlock(&mutex);
}

static inline int16_t aq_deq() {
	if (aq.qsize == 0) {
		//fprintf(stderr, "Audio Queue underflow!\n");
		return 0;
	}
	int16_t sample = aq.buffer[aq.front];
	aq.front = (aq.front + 1) % aq.bsize;
	aq.qsize--;
	return sample;
}

settings_t *smsp_settings_ptr() { return &settings; }

static void ma_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
	(void)pInput; // Don't take input
	
	if (aq.qsize < frameCount * CHANNELS) { return; }
	
	pthread_mutex_lock(&mutex);
	int16_t *out = (int16_t*)pOutput;
	for (int i = 0; i < frameCount * CHANNELS; i++) {
		out[i] = aq_deq();
	}
	pthread_mutex_unlock(&mutex);
}

void audio_init_ma() {
	// Set up "config" for playback
	ma_device_config config = ma_device_config_init(ma_device_type_playback);
	config.playback.pDeviceID = NULL; // NULL for default
	config.playback.format = ma_format_s16; // signed 16-bit integers
	config.playback.channels = CHANNELS; // SMS is stereo
	config.sampleRate = settings.audio_rate;
	config.dataCallback = ma_callback;
	config.pUserData = NULL;
	
	// Init hardware device
	if (ma_device_init(NULL, &config, &madevice) != MA_SUCCESS) {
        fprintf(stderr, "Failed to open playback device.\n");
    }
    else {
		fprintf(stdout, "Audio: %s, %dHz\n", madevice.playback.name, settings.audio_rate);
	}
	
	if (ma_device_start(&madevice) != MA_SUCCESS) {
		fprintf(stderr, "Failed to start playback device.\n");
		ma_device_uninit(&madevice);
	}
}

static void audio_init() {
	// First build the queue
	aq.front = 0;
	aq.rear = 0;
	aq.qsize = 0;
	aq.bsize = (settings.audio_rate / 60) * 8 * CHANNELS; // hardcoded 8 frames
	aq.buffer = (int16_t*)malloc(sizeof(int16_t) * aq.bsize);
	memset(aq.buffer, 0, sizeof(int16_t) * aq.bsize);
	memset(audiobuf, 0, BUFSIZE * sizeof(int16_t));
	
	audio_init_ma();
}

static void audio_deinit() {
	// Deinitialize audio
	ma_device_uninit(&madevice);
	if (aq.buffer) { free(aq.buffer); }
}

static void audio_push() {
	// Interleave the channels
	for (int i = 0; i < ((settings.audio_rate / 60) * CHANNELS); i++) {
		audiobuf[i * 2] = snd.output[0][i];
		audiobuf[i * 2 + 1] = snd.output[1][i];
	}
	aq_enq(audiobuf, (settings.audio_rate / 60) * CHANNELS);
}

#else 
void audio_init()
{

}
void audio_push()
{

}

void sound_callback( APP_S16* sample_pairs, int sample_pairs_count, void* user_data )
{
//	fprintf(stdout,"count %x buffer %x\n",sample_pairs_count,sample_pairs);

	for (int i = 0; i < sample_pairs_count; i++) {
		int seek = i;
		sample_pairs[(i*2)] = snd.output[0][seek];
		sample_pairs[(i*2)+1] = snd.output[1][seek];
//		sample_pairs[(i*2)+1] = snd.output[1][seek];
	}
}	

#endif 




void SMSPLUS_init(char const *filename)
{
	memset(&sms,0,sizeof(sms_t));
	smsp_gamedata_set((char*)filename);
	sms.console = strcmp(strrchr(filename, '.'), ".gg") ?
	CONSOLE_SMS : CONSOLE_GG;

	// Load ROM
	if(!load_rom((char*)filename)) {
	fprintf(stderr, "Error: Failed to load %s.\n", filename);
	exit(1);
	}

	fprintf(stdout, "CRC : %08X\n", cart.crc);
	//fprintf(stdout, "SHA1: %s\n", cart.sha1);
	fprintf(stdout, "SHA1: ");
	for (int i = 0; i < SHA1_DIGEST_SIZE; i++) {
	fprintf(stdout, "%02X", cart.sha1[i]);
	}
	fprintf(stdout, "\n");

	// Set defaults
	settings.video_scale = 2;
	settings.video_filter = 0;
	settings.audio_rate = 48000;
	settings.audio_fm = 1;
	settings.audio_fmtype = SND_EMU2413;
	settings.misc_region = TERRITORY_DOMESTIC;
	settings.misc_ffspeed = 2;
	
	// Override settings set in the .ini
	gbIniError err = gb_ini_parse("smsplus.ini", &smsp_ini_handler, &settings);
	if (err.type != GB_INI_ERROR_NONE) {
		fprintf(stderr, "Error: No smsplus.ini file found.\n");
	}
	
	// Create video buffer and grab the pointer
	smsp_video_create_buffer();
	uint8_t *pixels = smsp_video_pixels_ptr();
	
	// Set parameters for internal bitmap
	bitmap.width = VIDEO_WIDTH_SMS;
	bitmap.height = VIDEO_HEIGHT_SMS;
	bitmap.depth = 32;
	bitmap.granularity = 4;
	bitmap.data = pixels;
	bitmap.pitch = (bitmap.width * bitmap.granularity);
	bitmap.viewport.w = VIDEO_WIDTH_SMS;
	bitmap.viewport.h = VIDEO_HEIGHT_SMS;
	bitmap.viewport.x = 0x00;
	bitmap.viewport.y = 0x00;
	
	// Set parameters for internal sound
	snd.fm_which = settings.audio_fmtype;
	snd.fps = FPS_NTSC;
	snd.fm_clock = CLOCK_NTSC;
	snd.psg_clock = CLOCK_NTSC;
	snd.sample_rate = settings.audio_rate;
	snd.mixer_callback = NULL;
	
	sms.territory = settings.misc_region;
	if (sms.console != CONSOLE_GG) { sms.use_fm = settings.audio_fm; }
	
	// Initialize all systems and power on
	system_init();
	system_poweron();
	audio_init();

	fprintf(stderr, "sms init finished.\n");

}


void rect(int sx,int y,int w,int h,APP_U32 rgba)
{
APP_U32 *p = (APP_U32*)&pixels[0];
	p+=y*bitmap.width;
	for (int y=0;y<h;y++)
	{
		for (int x=0;x<w;x++)
		{
			p[sx+x]=rgba;
		}
		p+=bitmap.width;
	}
}



int app_proc( app_t* app, void* user_data ) {
    char const* filename = (char const*) user_data;

    app_title( app, "SMSPLUS" );
    app_interpolation( app, APP_INTERPOLATION_NONE );
    app_screenmode_t screenmode = APP_SCREENMODE_WINDOW;
    app_screenmode( app, screenmode );
    frametimer_t* frametimer = frametimer_create( NULL );
    frametimer_lock_rate( frametimer, 60 );

    app_yield( app );
    crtemu_pc_t* crtemu_pc = crtemu_pc_create( NULL );
    crtemu_pc_frame( crtemu_pc, (CRTEMU_U32*) a_crt_frame, 1024, 1024 );

    crtemu_t* crtemu = NULL;

    CRT_FRAME_U32* frame = (CRT_FRAME_U32*) malloc( CRT_FRAME_WIDTH * CRT_FRAME_HEIGHT * sizeof( CRT_FRAME_U32 ) );
    crt_frame( frame );

		SMSPLUS_init(filename);


#ifndef USE_MINI_AUDIO
		aq.bsize = (settings.audio_rate / 60) * CHANNELS; // hardcoded 8 frames
	//	fprintf(stdout,"set buffer size to %d",aq.bsize);
		app_sound(app,aq.bsize ,sound_callback,NULL);
#endif

    APP_U32 blank = 0;

    int w = VIDEO_WIDTH_SMS;
    int h = VIDEO_HEIGHT_SMS;
    int c;

		APP_U32* xbgr=(APP_U32*)pixels;
    APP_U64 start = app_time_count( app );

		int frames = 1;	

    int mode = 0;

    int exit = 0;
    while( !exit && app_yield( app ) != APP_STATE_EXIT_REQUESTED ) {
        frametimer_update( frametimer );

				bitmap.data = pixels;
				for (int i = 0; i < frames; i++) { system_frame(0); }
				BGR_to_RGB(w*h);
				audio_push();

        int newmode = mode;
        app_input_t a_input = app_input( app );
       	for( int i = 0; i < a_input.count; ++i ) {
					 
            if( a_input.events[i].type == APP_INPUT_KEY_DOWN ) {

							if (a_input.events[i].data.key == APP_KEY_Z) 			input.pad[0]|= INPUT_BUTTON1;
							if (a_input.events[i].data.key == APP_KEY_X) 			input.pad[0]|= INPUT_BUTTON2;
							if (a_input.events[i].data.key == APP_KEY_LEFT) 	input.pad[0]|= INPUT_LEFT;
							if (a_input.events[i].data.key == APP_KEY_RIGHT) 	input.pad[0]|= INPUT_RIGHT;
							if (a_input.events[i].data.key == APP_KEY_UP) 		input.pad[0]|= INPUT_UP;
							if (a_input.events[i].data.key == APP_KEY_DOWN) 	input.pad[0]|= INPUT_DOWN;

							if (a_input.events[i].data.key == APP_KEY_RETURN) input.system |= INPUT_PAUSE;
							if (a_input.events[i].data.key == APP_KEY_F1) 		input.system |= INPUT_RESET;
							if( a_input.events[i].data.key == APP_KEY_ESCAPE ) {
									exit = 1;
							}
							else if( a_input.events[i].data.key == APP_KEY_F11 ) {
									screenmode = screenmode == APP_SCREENMODE_WINDOW ? APP_SCREENMODE_FULLSCREEN : APP_SCREENMODE_WINDOW;
									app_screenmode( app, screenmode );
							}
            }
						if( a_input.events[i].type == APP_INPUT_KEY_UP ) {
							if (a_input.events[i].data.key == APP_KEY_Z) 			input.pad[0] &= ~INPUT_BUTTON1;
							if (a_input.events[i].data.key == APP_KEY_X) 			input.pad[0] &= ~INPUT_BUTTON2;
							if (a_input.events[i].data.key == APP_KEY_LEFT) 	input.pad[0] &= ~INPUT_LEFT;
							if (a_input.events[i].data.key == APP_KEY_RIGHT) 	input.pad[0] &= ~INPUT_RIGHT;
							if (a_input.events[i].data.key == APP_KEY_UP) 		input.pad[0] &= ~INPUT_UP;
							if (a_input.events[i].data.key == APP_KEY_DOWN) 	input.pad[0] &= ~INPUT_DOWN;

							if (a_input.events[i].data.key == APP_KEY_M) 
							{
								newmode = ( mode + 1 ) % 3;
							}

							if (a_input.events[i].data.key == APP_KEY_RETURN) input.system &= ~INPUT_PAUSE;
							if (a_input.events[i].data.key == APP_KEY_F1) 		input.system &= ~INPUT_RESET;
						}
        }

        if( mode != newmode ) {
            if( mode == 0 ) {
                crtemu_pc_destroy( crtemu_pc );
                crtemu_pc = NULL;
            } else if ( mode == 1 ) {
                crtemu_destroy( crtemu );
                crtemu = NULL;
            }
            mode = newmode;
            if( mode == 0 ) {
                crtemu_pc = crtemu_pc_create( NULL );
                crtemu_pc_frame( crtemu_pc, (CRTEMU_U32*) a_crt_frame, 1024, 1024 );
            } else if ( mode == 1 ) {
                crtemu = crtemu_create( NULL );
  	             crtemu_frame( crtemu, frame, CRT_FRAME_WIDTH, CRT_FRAME_HEIGHT);
            }
        }



        APP_U64 div = app_time_freq( app ) / 1000000ULL;
        APP_U64 t = ( app_time_count( app ) - start ) / div;

        if( mode == 0 ) {
            crtemu_pc_present( crtemu_pc, t, xbgr, w, h, 0xffffff, 0x181818 );
            app_present( app, NULL, w, h, 0xffffff, 0x000000 );
        } else if( mode == 1 ) {
            crtemu_present( crtemu, t, xbgr, w, h, 0xffffff, 0x181818 );
            app_present( app, NULL, w, h, 0xffffff, 0x000000 );
        } else {
            app_present( app, xbgr, w, h, 0xffffff, 0x181818 );
        }
    }

    if( mode == 0 ) {
        crtemu_pc_destroy( crtemu_pc );
    } else if( mode == 1 ) {
        crtemu_destroy( crtemu );
    }
 //   free( frame );
    frametimer_destroy( frametimer );
    return 0;
}


/*
-------------------------
    ENTRY POINT (MAIN)
-------------------------
*/     

#if defined( _WIN32 ) && !defined( __TINYC__ )
    #ifndef NDEBUG
        #pragma warning( push ) 
        #pragma warning( disable: 4619 ) // pragma warning : there is no warning number 'number'
        #pragma warning( disable: 4668 ) // 'symbol' is not defined as a preprocessor macro, replacing with '0' for 'directives'
        #include <crtdbg.h>
        #pragma warning( pop ) 
    #endif
#endif /* _WIN32 && !__TINYC__ */

int main( int argc, char** argv ) {
    (void) argc, (void) argv;
    #if defined( _WIN32 ) && !defined( __TINYC__ )
        #ifndef NDEBUG
            int flag = _CrtSetDbgFlag( _CRTDBG_REPORT_FLAG ); // Get current flag
            flag ^= _CRTDBG_LEAK_CHECK_DF; // Turn on leak-checking bit
            _CrtSetDbgFlag( flag ); // Set flag to the new value
            _CrtSetReportMode( _CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG );
            _CrtSetReportFile( _CRT_WARN, _CRTDBG_FILE_STDOUT );
            //_CrtSetBreakAlloc( 0 );
        #endif
    #endif

    char* filename = argc > 1 ? argv[ 1 ] : NULL;
    return app_run( app_proc, filename, NULL, NULL, NULL );
} 

#ifdef _WIN32
    // pass-through so the program will build with either /SUBSYSTEM:WINDOWS or /SUBSYSTEM:CONSOLE
    struct HINSTANCE__;
    int __stdcall WinMain( struct HINSTANCE__* a, struct HINSTANCE__* b, char* c, int d ) { 
        (void) a, (void) b, (void) c, (void) d; 
        return main( __argc, __argv ); 
    }
#endif /* _WIN32 */


/*
---------------------------------
    LIBRARIES IMPLEMENTATIONS
---------------------------------
*/

#define APP_IMPLEMENTATION
#ifdef _WIN32
    #define APP_WINDOWS
#else
    #define APP_SDL    
#endif
#define APP_LOG( ctx, level, message )
#include "app.h"

#define CRTEMU_IMPLEMENTATION
#include "crtemu.h"

#define CRT_FRAME_IMPLEMENTATION
#include "crt_frame.h"

#define CRTEMU_PC_IMPLEMENTATION
#define CRTEMU_PC_REPORT_SHADER_ERRORS
#include "crtemu_pc.h"

#define FRAMETIMER_IMPLEMENTATION
#include "frametimer.h"
#define	STBI_ONLY_PNG

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#pragma warning( push )
#pragma warning( disable: 4296 )
#include "stb_image.h"
#pragma warning( pop )