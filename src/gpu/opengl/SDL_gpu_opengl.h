/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#ifndef SDL_gpu_opengl_h_
#define SDL_gpu_opengl_h_

#ifdef SDL_GPU_OPENGL

#include "SDL.h"
#include "../SDL_sysgpu.h"

#include <GL/glcorearb.h>

// driver_data:
// SDL_CpuBuffer        -> buffer glid
// SDL_GpuBuffer        -> buffer glid
// SDL_GpuTexture       -> texture glid
// SDL_GpuShader        -> shader program glid
// SDL_GpuPipeline      -> vao glid<<32 | program pipeline glid
// SDL_GpuSampler       -> sampler glid
// SDL_GpuCommandBuffer ->
// SDL_GpuRenderPass    -> primitive<<48 | stride<<32 | fbo glid
// SDL_GpuBlitPass      ->
// SDL_GpuFence         ->

// grep -E 'gl[A-Z][0-Z]*\(' -o ./SDL_gpu_opengl.c | sort -u
#define OPENGL_FUNCTION_X \
GL_FN(GLBINDBUFFER                    , glBindBuffer                    ) \
GL_FN(GLBINDBUFFERRANGE               , glBindBufferRange               ) \
GL_FN(GLBINDFRAMEBUFFER               , glBindFramebuffer               ) \
GL_FN(GLBINDPROGRAMPIPELINE           , glBindProgramPipeline           ) \
GL_FN(GLBINDSAMPLER                   , glBindSampler                   ) \
GL_FN(GLBINDTEXTUREUNIT               , glBindTextureUnit               ) \
GL_FN(GLBINDVERTEXARRAY               , glBindVertexArray               ) \
GL_FN(GLBINDVERTEXBUFFER              , glBindVertexBuffer              ) \
GL_FN(GLBLENDCOLOR                    , glBlendColor                    ) \
GL_FN(GLBLENDEQUATIONSEPARATEI        , glBlendEquationSeparatei        ) \
GL_FN(GLBLENDFUNCSEPARATEI            , glBlendFuncSeparatei            ) \
GL_FN(GLBLITNAMEDFRAMEBUFFER          , glBlitNamedFramebuffer          ) \
GL_FN(GLCHECKNAMEDFRAMEBUFFERSTATUS   , glCheckNamedFramebufferStatus   ) \
GL_FN(GLCLEARNAMEDBUFFERDATA          , glClearNamedBufferData          ) \
GL_FN(GLCLEARNAMEDFRAMEBUFFERFV       , glClearNamedFramebufferfv       ) \
GL_FN(GLCLEARNAMEDFRAMEBUFFERIV       , glClearNamedFramebufferiv       ) \
GL_FN(GLCOLORMASKI                    , glColorMaski                    ) \
GL_FN(GLCOPYIMAGESUBDATA              , glCopyImageSubData              ) \
GL_FN(GLCOPYNAMEDBUFFERSUBDATA        , glCopyNamedBufferSubData        ) \
GL_FN(GLCREATEBUFFERS                 , glCreateBuffers                 ) \
GL_FN(GLCREATEFRAMEBUFFERS            , glCreateFramebuffers            ) \
GL_FN(GLCREATEPROGRAMPIPELINES        , glCreateProgramPipelines        ) \
GL_FN(GLCREATESAMPLERS                , glCreateSamplers                ) \
GL_FN(GLCREATESHADERPROGRAMV          , glCreateShaderProgramv          ) \
GL_FN(GLCREATETEXTURES                , glCreateTextures                ) \
GL_FN(GLCREATEVERTEXARRAYS            , glCreateVertexArrays            ) \
GL_FN(GLCULLFACE                      , glCullFace                      ) \
GL_FN(GLDELETEBUFFERS                 , glDeleteBuffers                 ) \
GL_FN(GLDELETEFRAMEBUFFERS            , glDeleteFramebuffers            ) \
GL_FN(GLDELETEPROGRAM                 , glDeleteProgram                 ) \
GL_FN(GLDELETEPROGRAMPIPELINES        , glDeleteProgramPipelines        ) \
GL_FN(GLDELETESAMPLERS                , glDeleteSamplers                ) \
GL_FN(GLDELETETEXTURES                , glDeleteTextures                ) \
GL_FN(GLDELETEVERTEXARRAYS            , glDeleteVertexArrays            ) \
GL_FN(GLDEPTHFUNC                     , glDepthFunc                     ) \
GL_FN(GLDEPTHMASK                     , glDepthMask                     ) \
GL_FN(GLDISABLE                       , glDisable                       ) \
GL_FN(GLDISABLEI                      , glDisablei                      ) \
GL_FN(GLDRAWARRAYS                    , glDrawArrays                    ) \
GL_FN(GLDRAWELEMENTS                  , glDrawElements                  ) \
GL_FN(GLENABLE                        , glEnable                        ) \
GL_FN(GLENABLEI                       , glEnablei                       ) \
GL_FN(GLENABLEVERTEXARRAYATTRIB       , glEnableVertexArrayAttrib       ) \
GL_FN(GLFRONTFACE                     , glFrontFace                     ) \
GL_FN(GLGENERATETEXTUREMIPMAP         , glGenerateTextureMipmap         ) \
GL_FN(GLGETERROR                      , glGetError                      ) \
GL_FN(GLGETPROGRAMINFOLOG             , glGetProgramInfoLog             ) \
GL_FN(GLGETPROGRAMIV                  , glGetProgramiv                  ) \
GL_FN(GLGETPROGRAMPIPELINEINFOLOG     , glGetProgramPipelineInfoLog     ) \
GL_FN(GLGETTEXTUREIMAGE               , glGetTextureImage               ) \
GL_FN(GLINVALIDATENAMEDFRAMEBUFFERDATA, glInvalidateNamedFramebufferData) \
GL_FN(GLMAPNAMEDBUFFER                , glMapNamedBuffer                ) \
GL_FN(GLNAMEDBUFFERSTORAGE            , glNamedBufferStorage            ) \
GL_FN(GLNAMEDFRAMEBUFFERDRAWBUFFERS   , glNamedFramebufferDrawBuffers   ) \
GL_FN(GLNAMEDFRAMEBUFFERTEXTURE       , glNamedFramebufferTexture       ) \
GL_FN(GLOBJECTLABEL                   , glObjectLabel                   ) \
GL_FN(GLPOLYGONMODE                   , glPolygonMode                   ) \
GL_FN(GLPOLYGONOFFSETCLAMP            , glPolygonOffsetClamp            ) \
GL_FN(GLPOPDEBUGGROUP                 , glPopDebugGroup                 ) \
GL_FN(GLPUSHDEBUGGROUP                , glPushDebugGroup                ) \
GL_FN(GLSAMPLERPARAMETERF             , glSamplerParameterf             ) \
GL_FN(GLSAMPLERPARAMETERFV            , glSamplerParameterfv            ) \
GL_FN(GLSAMPLERPARAMETERI             , glSamplerParameteri             ) \
GL_FN(GLSCISSOR                       , glScissor                       ) \
GL_FN(GLSTENCILFUNCSEPARATE           , glStencilFuncSeparate           ) \
GL_FN(GLSTENCILMASKSEPARATE           , glStencilMaskSeparate           ) \
GL_FN(GLSTENCILOPSEPARATE             , glStencilOpSeparate             ) \
GL_FN(GLTEXTUREPARAMETERI             , glTextureParameteri             ) \
GL_FN(GLTEXTURESTORAGE1D              , glTextureStorage1D              ) \
GL_FN(GLTEXTURESTORAGE2D              , glTextureStorage2D              ) \
GL_FN(GLTEXTURESTORAGE3D              , glTextureStorage3D              ) \
GL_FN(GLTEXTURESUBIMAGE1D             , glTextureSubImage1D             ) \
GL_FN(GLTEXTURESUBIMAGE2D             , glTextureSubImage2D             ) \
GL_FN(GLUNMAPNAMEDBUFFER              , glUnmapNamedBuffer              ) \
GL_FN(GLUSEPROGRAMSTAGES              , glUseProgramStages              ) \
GL_FN(GLVALIDATEPROGRAM               , glValidateProgram               ) \
GL_FN(GLVALIDATEPROGRAMPIPELINE       , glValidateProgramPipeline       ) \
GL_FN(GLVERTEXARRAYATTRIBBINDING      , glVertexArrayAttribBinding      ) \
GL_FN(GLVERTEXARRAYATTRIBFORMAT       , glVertexArrayAttribFormat       ) \
GL_FN(GLVERTEXARRAYATTRIBIFORMAT      , glVertexArrayAttribIFormat      ) \
GL_FN(GLVERTEXARRAYATTRIBLFORMAT      , glVertexArrayAttribLFormat      ) \
GL_FN(GLVIEWPORT                      , glViewport                      )

typedef struct OGL_GpuDevice
{
    SDL_GLContext *context;
    SDL_Window *window;
    SDL_bool dummy_window; // true if we own the window
    SDL_bool debug;
    GLuint fbo_backbuffer;
    GLuint texture_backbuffer;
    SDL_GpuPixelFormat texture_backbuffer_format;
    int w_backbuffer;
    int h_backbuffer;
    int swap_interval;
    GLint max_anisotropy;
    GLint max_texture_size;
    GLint max_texture_depth;
    GLsizeiptr max_buffer_size;
    GLint max_vertex_attrib;
    SDL_AtomicInt window_size_changed;

#ifndef GL_GLEXT_PROTOTYPES
#define GL_FN(T, N) PFN##T##PROC N;
    OPENGL_FUNCTION_X
#undef GL_FN
#endif
} OGL_GpuDevice;

#endif /* SDL_GPU_OPENGL */
#endif /* SDL_gpu_opengl_h_ */
