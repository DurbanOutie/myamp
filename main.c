#include <stdio.h>
#include "SDL2/SDL.h"


typedef void (*ClickFunc)(void);

typedef struct{

    //Not owned, DO NOT FREE
    SDL_Texture *texture;
    SDL_Rect src_rect_unpressed;
    SDL_Rect src_rect_pressed;
    SDL_Rect dst_rect;
    ClickFunc onClick;

} WinAmpSkinButton;

typedef enum{
    WASBTN_PREV = 0,
    WASBTN_PLAY,
    WASBTN_PAUSE,
    WASBTN_STOP,
    WASBTN_NEXT,
    WASBTN_EJECT,
    WASBTN_COUNT
} WinAmpSkinButtonId;

typedef struct{
    //Not owned, DO NOT FREE
    SDL_Texture *texture;
    WinAmpSkinButton knob;
    int num_frames;
    int x_offset;
    int y_offset;
    int w;
    int h;
    SDL_Rect dst_rect;
    float value;
    
} WinAmpSkinSlider;

typedef enum{
    WASSLD_VOLUME = 0,
    WASSLD_BALANCE,
    WASSLD_COUNT
} WinAmpSkinSliderId;


typedef struct{
    
    SDL_Texture *tex_main;    

    SDL_Texture *tex_cbuttons;    
    WinAmpSkinButton buttons[WASBTN_COUNT];

    SDL_Texture *tex_volume;    
    SDL_Texture *tex_balance;    
    WinAmpSkinSlider sliders[WASSLD_COUNT];

    WinAmpSkinButton *pressed;

} WinAmpSkin;


static SDL_AudioDeviceID audio_device = 0;
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_bool paused = SDL_TRUE;

static WinAmpSkin skin;

static Uint8 *wavbuf = NULL;
static Uint32 wavlen = 0;
static SDL_AudioSpec wavspec;
static SDL_AudioStream *stream = NULL;

static void SDLCALL feed_audio_device_callback(void *userdata, Uint8 *output_stream, int len){

    SDL_AudioStream *input_stream = (SDL_AudioStream *)SDL_AtomicGetPtr((void **) &stream);
    
    if(input_stream == NULL){
        SDL_memset(output_stream, '\0', len);
        return; 
    }
    const int num_converted_bytes = SDL_AudioStreamGet(input_stream, output_stream, len); 
    if(num_converted_bytes > 0){
        const float volume = skin.sliders[WASSLD_VOLUME].value;
        const float balance = skin.sliders[WASSLD_BALANCE].value;
        const int num_samples = num_converted_bytes/sizeof(float);
        float *samples = (float *)output_stream;

        // Always dealing with stereo for now
        SDL_assert((num_samples % 2) == 0);

        // change volume of audio playing
        if(volume != 1.0f){
            for(size_t i = 0; i < num_samples; ++i){
                samples[i] *= volume;
            }
        }

        // change balance of audio playing
        if(balance != 0.5f){
            for(size_t i = 0; i < num_samples; i += 2){

                const float weightLeft  = balance <= 0.5f ? 1.0f: 2.0f*(1.0f - balance);
                const float weightRight = balance >= 0.5f ? 1.0f: 2.0f*       (balance);

                samples[i]   *= weightLeft;
                samples[i+1] *= weightRight;
            }
        }
    }
    // now has number of bytes after feeding the device 
    len -= num_converted_bytes; 
    output_stream += num_converted_bytes;

    // fill the rest with silence
    if(len > 0){
        SDL_memset(output_stream, '\0', len);
    }
}

static void panic_and_abort(const char *title, const char *message){
    fprintf(stderr, "PANIC: %s ... %s\n", title, message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, window);
    SDL_Quit();
    exit(1);
}







static void stop_audio(void){

    // Make sure audio callback cant touch stream whilst freeing it 
    SDL_LockAudioDevice(audio_device); 
    if(stream){
        SDL_FreeAudioStream(stream);
        SDL_AtomicSetPtr((void **) &stream, NULL);
    }
    SDL_UnlockAudioDevice(audio_device);

    if(wavbuf){
        SDL_FreeWAV(wavbuf);
    }
    wavbuf = NULL;
    wavlen = 0;
}

static SDL_bool open_new_audio_file(const char *fname){
   

    SDL_AudioStream *tmp_stream = stream;
    // Make sure audio callback cant touch stream whilst freeing it 
    SDL_LockAudioDevice(audio_device); 
    SDL_AtomicSetPtr((void **) &stream, NULL);
    SDL_UnlockAudioDevice(audio_device);

    SDL_FreeAudioStream(tmp_stream);

    SDL_FreeWAV(wavbuf);
    wavbuf = NULL;
    wavlen = 0;


    if(SDL_LoadWAV(fname, &wavspec, &wavbuf, &wavlen) == NULL){
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LoadWAV Failed!", SDL_GetError(), window);
        goto failed;
    }
    tmp_stream = SDL_NewAudioStream(wavspec.format, wavspec.channels, wavspec.freq, AUDIO_F32, 2, 48000);
    if(!tmp_stream){
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Could not create Audio Stream!", SDL_GetError(), window);
        goto failed;
    }
    if(SDL_AudioStreamPut(tmp_stream, wavbuf, wavlen) == -1){
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AudioStreamPut Failed!", SDL_GetError(), window);
        goto failed;
    }
    if(SDL_AudioStreamFlush(tmp_stream) == -1){
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AudioStreamFlush Failed!", SDL_GetError(), window);
        goto failed;
    }


    // Make new `stream` available to audio callback thread
    SDL_LockAudioDevice(audio_device); 
    SDL_AtomicSetPtr((void **) &stream, tmp_stream);
    SDL_UnlockAudioDevice(audio_device);


    return SDL_TRUE;

failed:
    stop_audio();
    return SDL_FALSE;
}

SDL_Texture *load_texture(const char *fname){
    SDL_Surface *surface = SDL_LoadBMP(fname);
    if(!surface){
        return NULL;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture; // MAY BE NULL

}
static SDL_INLINE void init_skin_button(WinAmpSkinButton *btn, SDL_Texture *tex, ClickFunc onClick,
                                               const int w, const int h, 
                                               const int dx, const int dy, 
                                               const int sxu, const int syu, 
                                               const int sxp, const int syp){
        btn->texture = tex;
        btn->onClick = onClick;
        btn->src_rect_unpressed.x = sxu;
        btn->src_rect_unpressed.y = syu;
        btn->src_rect_unpressed.w = w;
        btn->src_rect_unpressed.h = h;
        btn->src_rect_pressed.x = sxp;
        btn->src_rect_pressed.y = syp;
        btn->src_rect_pressed.w = w;
        btn->src_rect_pressed.h = h;

        btn->dst_rect.x = dx;
        btn->dst_rect.y = dy;
        btn->dst_rect.w = w;
        btn->dst_rect.h = h;

}

static SDL_INLINE void init_skin_slider(WinAmpSkinSlider *slider, SDL_Texture *tex, 
                                               const int w, const int h, 
                                               const int dx, const int dy, 
                                               const int knob_w, const int knob_h,
                                               const int sxu, const int syu, 
                                               const int sxp, const int syp, 
                                               const int frame_x_offset, const int frame_y_offset,
                                               const int frame_w, const int frame_h,
                                               const int num_frames, const float value){


    init_skin_button(&slider->knob, tex, NULL, knob_w, knob_h, dx + (int)(((w - knob_w) * value) + 0.5f), dy, sxu, syu, sxp, syp);
    slider->texture = tex;
    slider->num_frames = num_frames;
    slider->value = value;
    slider->x_offset = frame_x_offset;
    slider->y_offset = frame_y_offset;
    slider->w = frame_w;
    slider->h = frame_h;

    slider->dst_rect.x = dx;
    slider->dst_rect.y = dy;
    slider->dst_rect.w = w;
    slider->dst_rect.h = h;


}

static void click_func_prev(void){
    printf("Prev Pressed\n");
    SDL_AudioStreamClear(stream);
    if(SDL_AudioStreamPut(stream, wavbuf, wavlen) == -1){
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AudioStreamPut Failed!", SDL_GetError(), window);
        stop_audio();
    }else if(SDL_AudioStreamFlush(stream) == -1){
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AudioStreamFlush Failed!", SDL_GetError(), window);
        stop_audio();
    }
}

static void click_func_pause(void){
    printf("Pause Pressed\n");
    paused = paused ? SDL_FALSE : SDL_TRUE;
    SDL_PauseAudioDevice(audio_device, paused);
}

static void click_func_stop(void){
    printf("Stop Pressed\n");
    stop_audio();
}

// FIXME: Use fname variable
static SDL_bool load_skin(WinAmpSkin *skin, const char *fname){

    SDL_zerop(skin);

    // FIXME: Hardcoded
    skin->tex_main = load_texture("base/MAIN.BMP");

    skin->tex_cbuttons = load_texture("base/CBUTTONS.BMP");
    init_skin_button(&skin->buttons[WASBTN_PREV],   skin->tex_cbuttons, click_func_prev,  23, 18,  16, 88,   0, 0,   0, 18);
    init_skin_button(&skin->buttons[WASBTN_PLAY],   skin->tex_cbuttons, NULL,  23, 18,  39, 88,  23, 0,  23, 18);
    init_skin_button(&skin->buttons[WASBTN_PAUSE],  skin->tex_cbuttons, click_func_pause, 23, 18,  62, 88,  46, 0,  46, 18);
    init_skin_button(&skin->buttons[WASBTN_STOP],   skin->tex_cbuttons, click_func_stop,  23, 18,  85, 88,  69, 0,  69, 18);
    init_skin_button(&skin->buttons[WASBTN_NEXT],   skin->tex_cbuttons, NULL,  22, 18, 108, 88,  92, 0,  92, 18);
    init_skin_button(&skin->buttons[WASBTN_EJECT],  skin->tex_cbuttons, NULL, 22, 16, 136, 89, 114, 0, 114, 16);

    skin->tex_volume = load_texture("base/VOLUME.BMP");
    init_skin_slider(&skin->sliders[WASSLD_VOLUME], skin->tex_volume, 68, 13, 107, 57, 14, 11, 15, 422, 0, 422, 0, 0, 68, 15, 28, 1.0f);

    skin->tex_balance = load_texture("base/BALANCE.BMP");
    init_skin_slider(&skin->sliders[WASSLD_BALANCE], skin->tex_balance, 38, 13, 177, 57, 14, 11, 15, 422, 0, 422, 9, 0, 47, 15, 28, 0.5f);

    return SDL_TRUE;

}

static void init_everything(int argc, char **argv){
    SDL_AudioSpec desired;
    
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1){
        panic_and_abort("SDL_Init failed", SDL_GetError());
    }

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 275, 116, 0);
    if(!window){
        panic_and_abort("SDL_CreateWindow Failed!", SDL_GetError());
    }
    // Tells SDL we want this event enabled
    // It is disabled by default due to having mem allocation
    // unfamiliar users may fall in the trap of not freeing the mem
    // resulting in memory leak <---

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if(!renderer){
        panic_and_abort("SDL_CreateRenderer Failed!", SDL_GetError());
    }


    // FIXME: Load a real thing
    if(!load_skin(&skin, "")){
        panic_and_abort("Load Initial Skin Failed!", SDL_GetError());
    }

    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_F32;
    desired.channels = 2;
    desired.samples = 4096;
    desired.callback = feed_audio_device_callback;

    audio_device = SDL_OpenAudioDevice( NULL, 0, &desired, NULL, 0);
    if(audio_device == 0){
        panic_and_abort("OpeAudioDevice Failed!", SDL_GetError());
    }

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE); 
    open_new_audio_file("music.wav");

}

static void draw_button(SDL_Renderer *renderer, WinAmpSkinButton *btn){
   
    const SDL_bool pressed = (skin.pressed == btn);
    if(btn->texture == NULL){
        if(pressed){
            SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        }else{
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        }
        SDL_RenderFillRect(renderer, &btn->dst_rect);
    }else{
        SDL_Rect *btn_rect = pressed ? &btn->src_rect_pressed : &btn->src_rect_unpressed;
        SDL_RenderCopy(renderer, btn->texture, btn_rect, &btn->dst_rect);
    }
}

static void draw_slider(SDL_Renderer *renderer, WinAmpSkinSlider *slider){


    SDL_assert(slider->value >= 0.0f);
    SDL_assert(slider->value <= 1.0f);
   
    if(slider->texture == NULL){
        const int color = (int)(slider->value * 255.0f);
        SDL_SetRenderDrawColor(renderer, 200, 0, color, 255);
        SDL_RenderFillRect(renderer, &slider->dst_rect);
    }else{
        const int frame_idx = ((int)((float)(slider->num_frames - 1) * slider->value));
        const int srcy = slider->y_offset + (frame_idx * slider->h);
        const SDL_Rect src_rect = { slider->x_offset, srcy, slider->dst_rect.w, slider->dst_rect.h};
        SDL_RenderCopy(renderer, slider->texture, &src_rect, &slider->dst_rect);
    }
    draw_button(renderer, &slider->knob);
}

static void draw_frame(SDL_Renderer *renderer, WinAmpSkin *skin){

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, skin->tex_main, NULL, NULL);

    int i;
    for(i = 0; i < SDL_arraysize(skin->buttons); ++i){
        draw_button(renderer, &skin->buttons[i]);
    }

    for(i = 0; i < SDL_arraysize(skin->sliders); ++i){
        draw_slider(renderer, &skin->sliders[i]);
    }
    SDL_RenderPresent(renderer);
}


static void deinit_everything(void){
    // FIXME: free_skin
    SDL_FreeWAV(wavbuf);


    SDL_CloseAudioDevice(audio_device);
    SDL_DestroyWindow(window);
    SDL_DestroyRenderer(renderer);
    SDL_Quit();
}

static void handle_slider_motion(WinAmpSkinSlider *slider, const SDL_Point *pt){
    if(skin.pressed == &slider->knob){
        const float new_val = ((float)pt->x - (float)slider->dst_rect.x) / (float)slider->dst_rect.w;
        const int new_knob_x = pt->x - (slider->knob.dst_rect.w/2);
        const int leftx = slider->dst_rect.x;
        const int rightx = leftx + slider->dst_rect.w - slider->knob.dst_rect.w;
        slider->knob.dst_rect.x = SDL_clamp(new_knob_x, leftx, rightx);

        // Make sure Mixer Thread isnt running when this value changes
        SDL_LockAudioDevice(audio_device);

        slider->value = SDL_clamp(new_val, 0.0f, 1.0f);
        SDL_UnlockAudioDevice(audio_device);
    }
}


static SDL_bool handle_events(WinAmpSkin *skin){
    SDL_Event e;
    while(SDL_PollEvent(&e)){
        switch(e.type){
            case SDL_QUIT:
                // Dont keep going
                return SDL_FALSE;
                break;
            case SDL_MOUSEBUTTONDOWN:{

                const SDL_Point pt = {e.button.x, e.button.y};

                if(e.button.button != SDL_BUTTON_LEFT){
                    break;
                }
                if(!skin->pressed){
                    for(int i = 0; i < SDL_arraysize(skin->buttons); ++i){
                        WinAmpSkinButton *btn = &skin->buttons[i];
                        if(SDL_PointInRect(&pt, &btn->dst_rect)){
                            skin->pressed = btn;
                            break;
                        }
                    }
                }

                if(!skin->pressed){
                    for(int i = 0; i < SDL_arraysize(skin->sliders); ++i){
                        WinAmpSkinSlider *slider = &skin->sliders[i];
                        if(SDL_PointInRect(&pt, &slider->dst_rect)){
                            skin->pressed = &slider->knob;
                            break;
                        }
                    }
                }
                if(skin->pressed){
                    SDL_CaptureMouse(SDL_TRUE);
                }

                break;
            }
            case SDL_MOUSEBUTTONUP:{

                if(e.button.button != SDL_BUTTON_LEFT){
                    break;
                }

                if(skin->pressed){
                    SDL_CaptureMouse(SDL_FALSE);
                    printf("Button Pressed::Function: %lX\n", (size_t)skin->pressed->onClick);
                    if(skin->pressed->onClick != NULL){
                        const SDL_Point pt = {e.button.x, e.button.y};
                        if(SDL_PointInRect(&pt, &skin->pressed->dst_rect)){
                            skin->pressed->onClick();
                        }
                    }
                    skin->pressed = NULL;
                }
                break;    
            }
            case SDL_MOUSEMOTION:{
                 const SDL_Point pt = {e.motion.x, e.motion.y};
                 for(int i = 0; i < SDL_arraysize(skin->sliders); ++i){
                     handle_slider_motion(&skin->sliders[i], &pt);
                 }
                 break;
             }
            case SDL_DROPFILE:
                open_new_audio_file(e.drop.file);
                SDL_free(e.drop.file);
                break;
        }
    }
    // Keep going
    return SDL_TRUE;
}


int main(int argc, char **argv){

    // will panic_and_abort if there are any issues
    init_everything(argc, argv); 
    while(handle_events(&skin)){
        draw_frame(renderer, &skin);
    }
    deinit_everything(); 
    return 0;
}


