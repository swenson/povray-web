//******************************************************************************
///
/// @file web/src/webconsole.cpp
///
/// Entry point for the WebAssembly (Emscripten) build of POV-Ray.
///
/// This is a trimmed-down variant of `vfe/unix/unixconsole.cpp`. Instead of
/// an SDL or text display it renders into an RGBA framebuffer that lives in
/// (shared) WebAssembly memory, so that the JavaScript side can poll it and
/// show a live preview on an HTML canvas. Cancellation is requested from
/// JavaScript via an exported function instead of POSIX signals.
///
/// @copyright
/// @parblock
///
/// Persistence of Vision Ray Tracer ('POV-Ray') version 3.8.
/// Copyright 1991-2021 Persistence of Vision Raytracer Pty. Ltd.
///
/// POV-Ray is free software: you can redistribute it and/or modify
/// it under the terms of the GNU Affero General Public License as
/// published by the Free Software Foundation, either version 3 of the
/// License, or (at your option) any later version.
///
/// POV-Ray is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU Affero General Public License for more details.
///
/// You should have received a copy of the GNU Affero General Public License
/// along with this program.  If not, see <http://www.gnu.org/licenses/>.
///
/// @endparblock
///
//******************************************************************************

// C++ variants of C standard header files
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// C++ standard header files
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Other library header files
#include <emscripten.h>
#include <unistd.h>

// from directory "vfe"
#include "vfe.h"
#include "unixoptions.h"

// from directory "source"
#include "backend/povray.h"

namespace pov_frontend
{
    // Referenced by the Unix display code; unused here but kept for parity.
    std::shared_ptr<Display> gDisplay;
}
// end of namespace pov_frontend

using namespace vfe;
using namespace vfePlatform;

enum ReturnValue
{
    RETURN_OK=0,
    RETURN_ERROR,
    RETURN_USER_ABORT
};

//******************************************************************************
// State shared with JavaScript.
//
// The JavaScript side runs on a different thread than the renderer, and reads
// these values directly out of the shared WebAssembly memory, so everything
// here is either atomic or only ever grows.
//******************************************************************************

static std::atomic<bool>     gCancelRender(false);
static std::atomic<uint8_t*> gFramePixels(nullptr);
static std::atomic<int>      gFrameWidth(0);
static std::atomic<int>      gFrameHeight(0);
static std::atomic<uint32_t> gFrameSerial(0);
static std::atomic<int>      gCurrentFrame(0);
static std::atomic<int>      gTotalFrames(0);

// Framebuffers are never freed while the program runs, so that JavaScript
// never reads from a buffer that has been handed back to the allocator.
static std::vector<std::unique_ptr<uint8_t[]>> gFrameBuffers;

extern "C"
{
    /// Request the current render to be cancelled.
    EMSCRIPTEN_KEEPALIVE void povweb_cancel() { gCancelRender = true; }

    /// RGBA8 preview framebuffer (`povweb_width() * povweb_height() * 4` bytes), or null.
    EMSCRIPTEN_KEEPALIVE uint8_t* povweb_framebuffer() { return gFramePixels; }
    EMSCRIPTEN_KEEPALIVE int povweb_width() { return gFrameWidth; }
    EMSCRIPTEN_KEEPALIVE int povweb_height() { return gFrameHeight; }

    /// Incremented whenever the preview framebuffer changes.
    EMSCRIPTEN_KEEPALIVE uint32_t povweb_frame_serial() { return gFrameSerial; }

    /// Animation progress.
    EMSCRIPTEN_KEEPALIVE int povweb_current_frame() { return gCurrentFrame; }
    EMSCRIPTEN_KEEPALIVE int povweb_total_frames() { return gTotalFrames; }
}

//******************************************************************************
// Display writing into the shared framebuffer.
//******************************************************************************

class WebDisplay final : public vfeDisplay
{
    public:

        WebDisplay(unsigned int w, unsigned int h, vfeSession *session, bool visible) :
            vfeDisplay(w, h, session, visible),
            mPixels(nullptr)
        {}

        virtual ~WebDisplay() override {}

        virtual void Initialise() override
        {
            const size_t size = size_t(GetWidth()) * size_t(GetHeight()) * 4;
            gFrameBuffers.emplace_back(new uint8_t[size]);
            mPixels = gFrameBuffers.back().get();
            std::fill(mPixels, mPixels + size, 0);
            gFrameWidth = int(GetWidth());
            gFrameHeight = int(GetHeight());
            gFramePixels = mPixels;
            ++gFrameSerial;
        }

        virtual void Close() override {}
        virtual void Show() override {}
        virtual void Hide() override {}

        virtual void DrawPixel(unsigned int x, unsigned int y, const RGBA8& colour) override
        {
            if (x >= GetWidth() || y >= GetHeight())
                return;
            Store(x, y, colour);
            ++gFrameSerial;
        }

        virtual void DrawRectangleFrame(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2, const RGBA8& colour) override
        {
            if (!Clip(x1, y1, x2, y2))
                return;
            for (unsigned int x = x1; x <= x2; ++x)
            {
                Store(x, y1, colour);
                Store(x, y2, colour);
            }
            for (unsigned int y = y1; y <= y2; ++y)
            {
                Store(x1, y, colour);
                Store(x2, y, colour);
            }
            ++gFrameSerial;
        }

        virtual void DrawFilledRectangle(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2, const RGBA8& colour) override
        {
            if (!Clip(x1, y1, x2, y2))
                return;
            for (unsigned int y = y1; y <= y2; ++y)
                for (unsigned int x = x1; x <= x2; ++x)
                    Store(x, y, colour);
            ++gFrameSerial;
        }

        virtual void DrawPixelBlock(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2, const RGBA8 *colour) override
        {
            const unsigned int w = x2 - x1 + 1;
            const unsigned int ox1 = x1, oy1 = y1;
            if (!Clip(x1, y1, x2, y2))
                return;
            for (unsigned int y = y1; y <= y2; ++y)
                for (unsigned int x = x1; x <= x2; ++x)
                    Store(x, y, colour[(y - oy1) * w + (x - ox1)]);
            ++gFrameSerial;
        }

        virtual void Clear() override
        {
            if (mPixels != nullptr)
                std::fill(mPixels, mPixels + size_t(GetWidth()) * size_t(GetHeight()) * 4, 0);
            ++gFrameSerial;
        }

    private:

        uint8_t* mPixels;

        inline void Store(unsigned int x, unsigned int y, const RGBA8& colour)
        {
            uint8_t* p = mPixels + (size_t(y) * GetWidth() + x) * 4;
            p[0] = colour.red;
            p[1] = colour.green;
            p[2] = colour.blue;
            p[3] = colour.alpha;
        }

        inline bool Clip(unsigned int& x1, unsigned int& y1, unsigned int& x2, unsigned int& y2)
        {
            if (mPixels == nullptr || x1 >= GetWidth() || y1 >= GetHeight() || x2 < x1 || y2 < y1)
                return false;
            if (x2 >= GetWidth())
                x2 = GetWidth() - 1;
            if (y2 >= GetHeight())
                y2 = GetHeight() - 1;
            return true;
        }
};

static vfeDisplay *WebDisplayCreator (unsigned int width, unsigned int height, vfeSession *session, bool visible)
{
    return new WebDisplay(width, height, session, visible);
}

//******************************************************************************
// Console output.
//******************************************************************************

static void PrintStatus (vfeSession *session)
{
    std::string str;
    vfeSession::MessageType type;

    while (session->GetNextCombinedMessage (type, str))
    {
        // The JavaScript front-end shows status lines separately from the
        // message log; prefix them so they can be told apart.
        if (type == vfeSession::mGenericStatus)
            fprintf (stdout, "\x01STATUS %s\n", str.c_str());
        else
            fprintf (stderr, "%s\n", str.c_str());
    }
}

static void PrintStatusChanged (vfeSession *session, State force = kUnknown)
{
    if (force == kUnknown)
        force = session->GetBackendState();
    switch (force)
    {
        case kParsing:
            fprintf (stderr, "==== [Parsing...] ==========================================================\n");
            fprintf (stdout, "\x01STATE parsing\n");
            break;
        case kRendering:
            fprintf (stderr, "==== [Rendering...] ========================================================\n");
            fprintf (stdout, "\x01STATE rendering\n");
            break;
        case kPausedRendering:
            fprintf (stderr, "==== [Paused...] ===========================================================\n");
            fprintf (stdout, "\x01STATE paused\n");
            break;
        default:
            break;
    }
}

static void PrintVersion(void)
{
    fprintf(stderr,
        "%s %s\n\n"
        "%s\n%s\n%s\n"
        "%s\n%s\n%s\n\n",
        PACKAGE_NAME, POV_RAY_VERSION,
        DISTRIBUTION_MESSAGE_1, DISTRIBUTION_MESSAGE_2, DISTRIBUTION_MESSAGE_3,
        POV_RAY_COPYRIGHT, DISCLAIMER_MESSAGE_1, DISCLAIMER_MESSAGE_2
    );
    fprintf(stderr,
        "Built-in features:\n"
        "  I/O restrictions:          %s\n"
        "  Display:                   %s\n"
        "  Supported image formats:   %s\n"
        "  Unsupported image formats: %s\n\n",
        BUILTIN_IO_RESTRICTIONS, BUILTIN_XWIN_DISPLAY, BUILTIN_IMG_FORMATS, MISSING_IMG_FORMATS
    );
    fprintf(stderr,
        "Compilation settings:\n"
        "  Build architecture:  %s\n"
        "  Built/Optimized for: %s\n"
        "  Compiler vendor:     %s\n"
        "  Compiler version:    %s\n"
        "  Compiler flags:      %s\n",
        BUILD_ARCH, BUILT_FOR, COMPILER_VENDOR, COMPILER_VERSION, CXXFLAGS
    );
}

static int ErrorExit(vfeSession *session)
{
    fprintf(stderr, "%s\n", session->GetErrorString());
    session->Shutdown();
    delete session;
    return RETURN_ERROR;
}

static void CancelRender(vfeSession *session)
{
    session->CancelRender();  // request the backend to cancel
    PrintStatus (session);
    while (session->GetBackendState() != kReady)  // wait for the render to effectively shut down
        Delay(10);
    PrintStatus (session);
}

int main (int argc, char **argv)
{
    vfeUnixSession   *session;
    vfeStatusFlags    flags;
    vfeRenderOptions  opts;
    ReturnValue       retval = RETURN_OK;

    gCancelRender = false;

    session = new vfeUnixSession();
    if (session->Initialize(nullptr, nullptr) != vfeNoError)
        return ErrorExit(session);

    // Default number of render threads: the number of logical CPUs reported
    // by the browser (`navigator.hardwareConcurrency`).
    int nthreads = int(sysconf(_SC_NPROCESSORS_ONLN));
    if (nthreads < 1)
        nthreads = 4;
    opts.SetThreadCount(nthreads);

    // process command-line options
    session->GetUnixOptions()->ProcessOptions(&argc, &argv);
    if (session->GetUnixOptions()->isOptionSet("general", "help"))
    {
        session->Shutdown() ;
        PrintStatus (session) ;
        session->GetUnixOptions()->PrintOptions();
        delete session;
        return RETURN_OK;
    }
    else if (session->GetUnixOptions()->isOptionSet("general", "version"))
    {
        session->Shutdown() ;
        PrintVersion();
        delete session;
        return RETURN_OK;
    }
    else if (session->GetUnixOptions()->isOptionSet("general", "benchmark"))
    {
        fprintf(stderr, "%s: the built-in benchmark is not supported by the web build\n", PACKAGE);
        session->Shutdown();
        delete session;
        return RETURN_ERROR;
    }

    // process INI settings
    session->SetDisplayCreator(WebDisplayCreator);
    session->GetUnixOptions()->Process_povray_ini(opts);
    while (*++argv)
        opts.AddCommand (*argv);

    // set all options and start rendering
    if (session->SetOptions(opts) != vfeNoError)
    {
        fprintf(stderr,"\nProblem with option setting\n");
        return ErrorExit(session);
    }
    if (session->StartRender() != vfeNoError)
        return ErrorExit(session);

    // main render loop
    session->SetEventMask(stBackendStateChanged);  // immediately notify this event
    while (((flags = session->GetStatus(true, 200)) & stRenderShutdown) == 0)
    {
        if (gCancelRender)
        {
            fprintf(stderr, "\n%s: render cancelled by user\n", PACKAGE);
            CancelRender(session);
            break;
        }

        if (flags & stAnimationStatus)
        {
            gCurrentFrame = session->GetCurrentFrame();
            gTotalFrames = session->GetTotalFrames();
            fprintf(stderr, "\nRendering frame %d of %d (#%d)\n", session->GetCurrentFrame(), session->GetTotalFrames(), session->GetCurrentFrameId());
            fprintf(stdout, "\x01" "FRAME %d %d\n", session->GetCurrentFrame(), session->GetTotalFrames());
        }
        if (flags & stAnyMessage)
            PrintStatus (session);
        if (flags & stBackendStateChanged)
            PrintStatusChanged (session);
    }

    if (session->Succeeded() == false)
        retval = gCancelRender ? RETURN_USER_ABORT : RETURN_ERROR;
    session->Shutdown();
    PrintStatus (session);
    delete session;

    return retval;
}
