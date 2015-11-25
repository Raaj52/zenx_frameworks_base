/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "BakedOpRenderer.h"

#include "Caches.h"
#include "Glop.h"
#include "GlopBuilder.h"
#include "renderstate/OffscreenBufferPool.h"
#include "renderstate/RenderState.h"
#include "utils/GLUtils.h"
#include "VertexBuffer.h"

#include <algorithm>

namespace android {
namespace uirenderer {

OffscreenBuffer* BakedOpRenderer::startTemporaryLayer(uint32_t width, uint32_t height) {
    LOG_ALWAYS_FATAL_IF(mRenderTarget.offscreenBuffer, "already has layer...");

    OffscreenBuffer* buffer = mRenderState.layerPool().get(mRenderState, width, height);
    startRepaintLayer(buffer, Rect(width, height));
    return buffer;
}

void BakedOpRenderer::startRepaintLayer(OffscreenBuffer* offscreenBuffer, const Rect& repaintRect) {
    LOG_ALWAYS_FATAL_IF(mRenderTarget.offscreenBuffer, "already has layer...");

    mRenderTarget.offscreenBuffer = offscreenBuffer;

    // create and bind framebuffer
    mRenderTarget.frameBufferId = mRenderState.genFramebuffer();
    mRenderState.bindFramebuffer(mRenderTarget.frameBufferId);

    // attach the texture to the FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
            offscreenBuffer->texture.id, 0);
    LOG_ALWAYS_FATAL_IF(GLUtils::dumpGLErrors(), "startLayer FAILED");
    LOG_ALWAYS_FATAL_IF(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE,
            "framebuffer incomplete!");

    // Change the viewport & ortho projection
    setViewport(offscreenBuffer->viewportWidth, offscreenBuffer->viewportHeight);

    clearColorBuffer(repaintRect);
}

void BakedOpRenderer::endLayer() {
    mRenderTarget.offscreenBuffer->updateMeshFromRegion();
    mRenderTarget.offscreenBuffer = nullptr;

    // Detach the texture from the FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    LOG_ALWAYS_FATAL_IF(GLUtils::dumpGLErrors(), "endLayer FAILED");
    mRenderState.deleteFramebuffer(mRenderTarget.frameBufferId);
    mRenderTarget.frameBufferId = -1;
}

void BakedOpRenderer::startFrame(uint32_t width, uint32_t height, const Rect& repaintRect) {
    mRenderState.bindFramebuffer(0);
    setViewport(width, height);
    mCaches.clearGarbage();

    if (!mOpaque) {
        clearColorBuffer(repaintRect);
    }
}

void BakedOpRenderer::endFrame() {
    mCaches.pathCache.trim();
    mCaches.tessellationCache.trim();

#if DEBUG_OPENGL
    GLUtils::dumpGLErrors();
#endif

#if DEBUG_MEMORY_USAGE
    mCaches.dumpMemoryUsage();
#else
    if (Properties::debugLevel & kDebugMemory) {
        mCaches.dumpMemoryUsage();
    }
#endif
}

void BakedOpRenderer::setViewport(uint32_t width, uint32_t height) {
    mRenderTarget.viewportWidth = width;
    mRenderTarget.viewportHeight = height;
    mRenderTarget.orthoMatrix.loadOrtho(width, height);

    mRenderState.setViewport(width, height);
    mRenderState.blend().syncEnabled();
}

void BakedOpRenderer::clearColorBuffer(const Rect& rect) {
    if (Rect(mRenderTarget.viewportWidth, mRenderTarget.viewportHeight).contains(rect)) {
        // Full viewport is being cleared - disable scissor
        mRenderState.scissor().setEnabled(false);
    } else {
        // Requested rect is subset of viewport - scissor to it to avoid over-clearing
        mRenderState.scissor().setEnabled(true);
        mRenderState.scissor().set(rect.left, mRenderTarget.viewportHeight - rect.bottom,
                rect.getWidth(), rect.getHeight());
    }
    glClear(GL_COLOR_BUFFER_BIT);
    if (!mRenderTarget.frameBufferId) mHasDrawn = true;
}

Texture* BakedOpRenderer::getTexture(const SkBitmap* bitmap) {
    Texture* texture = mRenderState.assetAtlas().getEntryTexture(bitmap);
    if (!texture) {
        return mCaches.textureCache.get(bitmap);
    }
    return texture;
}

void BakedOpRenderer::renderGlop(const BakedOpState& state, const Glop& glop) {
    bool useScissor = state.computedState.clipSideFlags != OpClipSideFlags::None;
    mRenderState.scissor().setEnabled(useScissor);
    if (useScissor) {
        const Rect& clip = state.computedState.clipRect;
        mRenderState.scissor().set(clip.left, mRenderTarget.viewportHeight - clip.bottom,
            clip.getWidth(), clip.getHeight());
    }
    if (mRenderTarget.offscreenBuffer) { // TODO: not with multi-draw
        // register layer damage to draw-back region
        const Rect& uiDirty = state.computedState.clippedBounds;
        android::Rect dirty(uiDirty.left, uiDirty.top, uiDirty.right, uiDirty.bottom);
        mRenderTarget.offscreenBuffer->region.orSelf(dirty);
    }
    mRenderState.render(glop, mRenderTarget.orthoMatrix);
    if (!mRenderTarget.frameBufferId) mHasDrawn = true;
}

} // namespace uirenderer
} // namespace android
