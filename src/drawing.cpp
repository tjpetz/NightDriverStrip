//+--------------------------------------------------------------------------
//
// File:        drawing.cpp
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.  
//
// This file is part of the NightDriver software project.
//
//    NightDriver is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//   
//    NightDriver is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//   
//    You should have received a copy of the GNU General Public License
//    along with Nightdriver.  It is normally found in copying.txt
//    If not, see <https://www.gnu.org/licenses/>.
//
// Description:
//
//    Main draw loop and rendering code
//
// History:     May-11-2021         Davepl      Commented
//              Nov-02-2022         Davepl      Broke up into multiple functions
//
//---------------------------------------------------------------------------

#include "globals.h"
#include "effectmanager.h"
#include "ledbuffer.h"
#include "ntptimeclient.h"
#include "remotecontrol.h"
#include <mutex>
#include <ArduinoOTA.h>             // Over-the-air helper object so we can be flashed via WiFi
#include "ntptimeclient.h"
#include "effects/matrix/spectrumeffects.h"

#ifdef USESTRIP
#include "ledstripgfx.h"
#endif

#ifdef USEMATRIX
#include "ledstripgfx.h"
#include "ledmatrixgfx.h"
#endif

CRGB g_SinglePixel = CRGB::Blue;
CLEDController * g_ledSinglePixel;

// The g_buffer_mutex is a global mutex used to protect access while adding or removing frames
// from the led buffer.  

extern std::mutex         g_buffer_mutex;

DRAM_ATTR std::unique_ptr<LEDBufferManager> g_apBufferManager[NUM_CHANNELS];
DRAM_ATTR std::unique_ptr<EffectManager<GFXBase>> g_pEffectManager;
double volatile           g_FreeDrawTime = 0.0;

extern uint32_t           g_FPS;
extern AppTime            g_AppTime;
extern bool               g_bUpdateStarted;
extern double             g_Brite;
extern uint32_t           g_Watts; 
extern const CRGBPalette256 vuPaletteGreen;

void ShowTM1814();

// DrawLoop
//
// Pull packets from the Wifi buffer if they've come due and draw them - if it-'s a few seconds without a WiFi frame,
// we will draw the local effect instead

DRAM_ATTR uint64_t g_msLastWifiDraw  = 0;
DRAM_ATTR double   g_BufferAgeOldest = 0;
DRAM_ATTR double   g_BufferAgeNewest = 0;

DRAM_ATTR uint8_t  g_Brightness      = 255;
DRAM_ATTR uint8_t  g_Fader           = 255;

// MatrixPreDraw
//
// Gets the matrix ready for the effect or wifi to render into

void MatrixPreDraw()
{
     #if USEMATRIX
        // We treat the internal matrix buffer as our own little playground to draw in, but that assumes they're
        // both 24-bits RGB triplets.  Or at least the same size!

        static_assert( sizeof(CRGB) == sizeof(LEDMatrixGFX::SM_RGB), "Code assumes 24 bits in both places" );

        EVERY_N_MILLIS(MILLIS_PER_FRAME)
        {
            #if SHOW_FPS_ON_MATRIX
                LEDMatrixGFX::backgroundLayer.setFont(gohufont11);
                // 3 is half char width at curret font size, 5 is half the height.
                string output = "FPS: " + std::to_string(g_FPS);
                LEDMatrixGFX::backgroundLayer.drawString(MATRIX_WIDTH / 2 -  (3 * output.length()), MATRIX_HEIGHT / 2 - 5, rgb24(255,255,255), rgb24(0,0,0), output.c_str());    
            #endif

            GFXBase * graphics = (GFXBase *)(*g_pEffectManager)[0].get();

            LEDMatrixGFX * pMatrix = (LEDMatrixGFX *) graphics;
            LEDMatrixGFX::MatrixSwapBuffers(g_pEffectManager->GetCurrentEffect()->RequiresDoubleBuffering(), pMatrix->GetCaptionTransparency() > 0);
            pMatrix->setLeds(LEDMatrixGFX::GetMatrixBackBuffer());
            LEDMatrixGFX::titleLayer.setFont(font3x5);

            if (pMatrix->GetCaptionTransparency() > 0.00) 
            {
                uint8_t brite = (uint8_t)(pMatrix->GetCaptionTransparency() * 255.0);
                LEDMatrixGFX::titleLayer.setBrightness(brite);                // 255 would obscure it entirely
                debugV("Caption: %d", brite);

                rgb24 chromaKeyColor = rgb24(255,0,255);
                rgb24 shadowColor = rgb24(0,0,0);
                rgb24 titleColor = rgb24(255,255,255);
                
                LEDMatrixGFX::titleLayer.setChromaKeyColor(chromaKeyColor);
                LEDMatrixGFX::titleLayer.enableChromaKey(true);
                LEDMatrixGFX::titleLayer.setFont(font6x10);
                LEDMatrixGFX::titleLayer.fillScreen(chromaKeyColor);

                const size_t kCharWidth = 6;
                const size_t kCharHeight = 10;

                const auto caption = pMatrix->GetCaption();

                int y = MATRIX_HEIGHT - 2 - kCharHeight;
                int w = strlen(caption) * kCharWidth;
                int x = (MATRIX_WIDTH / 2) - (w / 2); 

                LEDMatrixGFX::titleLayer.drawString(x-1, y,   shadowColor, caption);
                LEDMatrixGFX::titleLayer.drawString(x+1, y,   shadowColor, caption);
                LEDMatrixGFX::titleLayer.drawString(x,   y-1, shadowColor, caption);
                LEDMatrixGFX::titleLayer.drawString(x,   y+1, shadowColor, caption);
                LEDMatrixGFX::titleLayer.drawString(x,   y,   titleColor,  caption);
            }
            else 
            {
                LEDMatrixGFX::titleLayer.enableChromaKey(false);
                LEDMatrixGFX::titleLayer.setBrightness(0);
            }   
        }
    #endif
}

// WiFiDraw
//
// Draws forom WiFi color data if available, returns pixels drawn this frame

uint16_t WiFiDraw()        
{
    uint16_t pixelsDrawn = 0;

    timeval tv;
    gettimeofday(&tv, nullptr);

    std::lock_guard<std::mutex> guard(g_buffer_mutex);

    for (int iChannel = 0; iChannel < NUM_CHANNELS; iChannel++)
    {
        // Pull buffers out of the queue.  Changing the 'while' to an 'if' would cause it to draw every frame if it got behind, but when
        // written as 'while' it will pull frames until it gets one that is current.

        if (false == g_apBufferManager[iChannel]->IsEmpty())
        {
            std::shared_ptr<LEDBuffer> pBuffer;
            if (NTPTimeClient::HasClockBeenSet() == false)
            {
                pBuffer = g_apBufferManager[iChannel]->GetOldestBuffer();
            }
            else
            {
                // Chew through ALL frames older than now, ignoring all but the last of them
                while (!g_apBufferManager[iChannel]->IsEmpty() && g_apBufferManager[iChannel]->PeekOldestBuffer()->IsBufferOlderThan(tv))
                    pBuffer = g_apBufferManager[iChannel]->GetOldestBuffer();
            }
        
            if (pBuffer)
            {
                g_AppTime.NewFrame();
                g_msLastWifiDraw = micros();  
                pixelsDrawn = pBuffer->Length();
                debugV("Calling LEDBuffer::Draw from wire with %d/%d pixels.", pixelsDrawn, NUM_LEDS);
                pBuffer->DrawBuffer();
            }
        }
                        
        if (false == g_apBufferManager[iChannel]->IsEmpty())
        {
            auto pOldest = g_apBufferManager[iChannel]->PeekOldestBuffer();
            auto pNewest = g_apBufferManager[iChannel]->PeekNewestBuffer();                    
            g_BufferAgeNewest = (pNewest->Seconds() + pNewest->MicroSeconds() / (double) MICROS_PER_SECOND) - g_AppTime.CurrentTime();
            g_BufferAgeOldest = (pOldest->Seconds() + pOldest->MicroSeconds() / (double) MICROS_PER_SECOND) - g_AppTime.CurrentTime();
        }
        else
        {
            g_BufferAgeNewest = g_BufferAgeOldest = 0;
        }
    }  
    return pixelsDrawn;        
}

// LocalDraw
// 
// Draws from effets table rather than from WiFi data

uint16_t LocalDraw()
{
    GFXBase * graphics = (GFXBase *)(*g_pEffectManager)[0].get();
    
    if (nullptr == g_pEffectManager)
    {
        debugW("Drawing before g_pEffectManager is ready, so delaying...");
        delay(100);
        return 0;
    }
    else if (g_pEffectManager->EffectCount() > 0)
    {
         // If we've never drawn from wifi before, now would also be a good time to local draw
        if (g_msLastWifiDraw == 0 || (micros() - g_msLastWifiDraw > (TIME_BEFORE_LOCAL * MICROS_PER_SECOND)))  
        {
            g_AppTime.NewFrame();       // Start a new frame, record the time, calc deltaTime, etc.
            g_pEffectManager->Update(); // Draw the current built in effect

            #if USEMATRIX
                auto spectrum = GetSpectrumAnalyzer(0);
                if (g_pEffectManager->IsVUVisible())
                    ((SpectrumAnalyzerEffect *)spectrum.get())->DrawVUMeter(graphics, 0, &vuPaletteGreen);
            #endif
        }
        else
        {
            debugV("Not drawing local effect because last wifi draw was %lf seconds ago.", (micros()-g_msLastWifiDraw) / (double) MICROS_PER_SECOND);
        }
        return NUM_LEDS;
    }
    return 0;
}

// ShowStrip
//
// ShowStrip sends the data to the LED strip.  If its fewer than the size of the strip, we only send that many.

void ShowStrip(uint16_t numToShow)
{
    // If we've drawn anything from either source, we can now show it

    if (FastLED.count() == 0)
    {
        debugW("Draw loop is drawing before LEDs are ready, so delaying 100ms...");
        delay(100);
    }
    else
    {
        if (numToShow > 0)
        {
            // debugV("Telling FastLED that we'll be drawing %d pixels\n", numToShow);
            
            for (int i  = 0; i < NUM_CHANNELS; i++)
            {
                LEDStripGFX * pStrip = (LEDStripGFX *)(*g_pEffectManager)[i].get();
                FastLED[i].setLeds(pStrip->leds, numToShow);
            }

            FastLED.show(g_Fader);

            g_FPS = FastLED.getFPS(); 
            g_Brite = 100.0 * calculate_max_brightness_for_power_mW(g_Brightness, POWER_LIMIT_MW) / 255;
            g_Watts = calculate_unscaled_power_mW( ((LEDStripGFX *)(*g_pEffectManager)[0].get())->leds, numToShow ) / 1000;    // 1000 for mw->W
        }
        else
        {
            debugV("Draw loop ended without a draw.");
        }
    }
}

// DelayUntilNextFrame
//
// Waits patiently until its time to draw the next frame, up to one second max

void DelayUntilNextFrame(double frameStartTime, uint16_t localPixelsDrawn, uint16_t wifiPixelsDrawn)
{
    // Delay enough to slow down to the desired framerate
    // BUGBUG (davepl) This uses the current effect from the effects table, so its used even for wifi frames

    #if MILLIS_PER_FRAME == 0
        if (localPixelsDrawn > 0)
        {
            const double minimumFrameTime = 1.0/g_pEffectManager->GetCurrentEffect()->DesiredFramesPerSecond();
            double elapsed = g_AppTime.CurrentTime() - frameStartTime;
            if (elapsed < minimumFrameTime)
            {
                g_FreeDrawTime = std::min(1.0, (minimumFrameTime - elapsed));
                delay(g_FreeDrawTime * MILLIS_PER_SECOND);
            }
        }
        else if (wifiPixelsDrawn > 0)
        {
            // Sleep up to 1/20th second, depending on how far away the next frame we need to service is

            double t = 0.05;         
            for (int iChannel = 0; iChannel < NUM_CHANNELS; iChannel++)
            {
                auto pOldest = g_apBufferManager[iChannel]->PeekOldestBuffer();
                if (pOldest)
                    t = std::min(t, (pOldest->Seconds() + pOldest->MicroSeconds() / (double) MICROS_PER_SECOND) - g_AppTime.CurrentTime());
            }

            g_FreeDrawTime = t;
            if (g_FreeDrawTime > 0.0)
                delay(g_FreeDrawTime * MILLIS_PER_SECOND);
            else
                g_FreeDrawTime = 0.0;

        }
        else
        {
            // Nothing drawn this pass - check back soon
            g_FreeDrawTime = .001;
            delay(1);
        }

    #endif
}

// ShowOnboardLED
//
// If the board has an onboard LED, this will update it to show some activity from the draw

void ShowOnboardRGBLED()
{
    // Some boards have onboard PWM RGB LEDs, so if defined, we color them here.  If we're doing audio,
    // the color maps to the sound level.  If no audio, it shows the middle LED color from the strip.

    #ifdef ONBOARD_LED_R
        #ifdef ENABLE_AUDIO
            CRGB c = ColorFromPalette(HeatColors_p, gVURatioFade / 2.0 * 255); 
            ledcWrite(1, 255 - c.r ); // write red component to channel 1, etc.
            ledcWrite(2, 255 - c.g );
            ledcWrite(3, 255 - c.b );
        #else
            int iLed = NUM_LEDS/2;
            ledcWrite(1, 255 - graphics->leds[iLed].r ); // write red component to channel 1, etc.
            ledcWrite(2, 255 - graphics->leds[iLed].g );
            ledcWrite(3, 255 - graphics->leds[iLed].b );
        #endif
    #endif
}

// PrepareOnboardPixel
//
// Do any setup required for the onboard pixel, if we have one

void PrepareOnboardPixel()
{
    #ifdef ONBOARD_PIXEL_POWER
        g_ledSinglePixel = &FastLED.addLeds<WS2812B, ONBOARD_PIXEL_DATA, ONBOARD_PIXEL_ORDER>(&g_SinglePixel, 1);
        pinMode(ONBOARD_PIXEL_POWER, OUTPUT);
        digitalWrite(ONBOARD_PIXEL_POWER, HIGH);
    #endif
}

void ShowOnboardPixel()
{
    // Some boards have onboard PWM RGB LEDs, so if defined, we color them here.  If we're doing audio,
    // the color maps to the sound level.  If no audio, it shows the middle LED color from the strip.

    #ifdef ONBOARD_PIXEL_POWER
        g_SinglePixel = FastLED[0].leds()[0];
    #endif
}

// DrawLoopTaskEntry
// 
// Main draw loop entry point

void IRAM_ATTR DrawLoopTaskEntry(void *)
{
    
    debugI(">> DrawLoopTaskEntry\n");

    // Initialize our graphics and the first effect

    PrepareOnboardPixel();

    GFXBase * graphics = (GFXBase *)(*g_pEffectManager)[0].get();
    graphics->Setup();

    #if USEMATRIX
        // We don't need color correction on the chromakey'd title layer
        LEDMatrixGFX::titleLayer.enableColorCorrection(false);

        // Starting the effect might need to draw, so we need to set the leds up before doing so
        LEDMatrixGFX * pMatrix = (LEDMatrixGFX *)graphics;
        pMatrix->setLeds(LEDMatrixGFX::GetMatrixBackBuffer());
        auto spectrum = GetSpectrumAnalyzer(0);
    #endif
    g_pEffectManager->StartEffect();
    
    // Run the draw loop

    for (;;)
    {
        // Loop through each of the channels and see if they have a current frame that needs to be drawn
        
        uint16_t localPixelsDrawn   = 0;
        uint16_t wifiPixelsDrawn    = 0;
        double   frameStartTime     = g_AppTime.CurrentTime();

        #if USEMATRIX
            MatrixPreDraw();
        #endif

        if (WiFi.isConnected())
            wifiPixelsDrawn = WiFiDraw();

        // If we didn't draw now, and it's been a while since we did, and we have at least one local effect, then draw the local effect instead
        
        if (wifiPixelsDrawn == 0)
            localPixelsDrawn = LocalDraw();

        #if USESTRIP
            if (wifiPixelsDrawn)
                ShowStrip(wifiPixelsDrawn);
            else if (localPixelsDrawn)
                ShowStrip(localPixelsDrawn);
        #endif

        // If the module has onboard LEDs, we support a couple of different types, and we set it to be the same as whatever
        // is on LED #0 of Channel #0.

        ShowOnboardPixel();
        ShowOnboardRGBLED();

        DelayUntilNextFrame(frameStartTime, localPixelsDrawn, wifiPixelsDrawn);

        // Once an OTA flash update has started, we don't want to hog the CPU or it goes quite slowly,
        // so we'll pause to share the CPU a bit once the update has begun

        if (g_bUpdateStarted)
            delay(100);
        
        // If we didn't draw anything, we near-busy-wait so that we are continually checking the clock for an packet
        // whose time has come

        yield();
    }
}
