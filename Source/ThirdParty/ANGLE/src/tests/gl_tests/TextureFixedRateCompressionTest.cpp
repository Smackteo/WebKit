//
// Copyright 2024 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// TextureFixedRateCompressionTest: Tests for GL_EXT_texture_storage_compression

#include "test_utils/ANGLETest.h"
#include "test_utils/gl_raii.h"

namespace angle
{

static constexpr GLint kDefaultAttribList[3][3] = {
    {GL_NONE, GL_NONE, GL_NONE},
    {GL_SURFACE_COMPRESSION_EXT, GL_SURFACE_COMPRESSION_FIXED_RATE_NONE_EXT, GL_NONE},
    {GL_SURFACE_COMPRESSION_EXT, GL_SURFACE_COMPRESSION_FIXED_RATE_DEFAULT_EXT, GL_NONE},
};

// Return its name by value for easy printing
static const char *Name(int value)
{
#define NAME(ERR) \
    case ERR:     \
        return #ERR;
    switch (value)
    {
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_NONE_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_DEFAULT_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_1BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_2BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_3BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_4BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_5BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_6BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_7BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_8BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_9BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_10BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_11BPC_EXT)
        NAME(GL_SURFACE_COMPRESSION_FIXED_RATE_12BPC_EXT)
    }
    return "UNKNOW";
#undef NAME
}

class TextureFixedRateCompressionTest : public ANGLETest<>
{
  protected:
    void invalidTestHelper(const GLint *attribs);
    void basicTestHelper(const GLint *attribs);
    void RenderToTexture(const GLenum internalFormat, const GLint *attribs);
};

void TextureFixedRateCompressionTest::invalidTestHelper(const GLint *attribs)
{
    GLTexture tex;
    glBindTexture(GL_TEXTURE_2D, tex);
    ASSERT_GL_NO_ERROR();

    glTexStorageAttribs2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16, attribs);
    ASSERT_GL_NO_ERROR();

    /* Query compression rate */
    GLint compressRate = GL_SURFACE_COMPRESSION_FIXED_RATE_NONE_EXT;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_SURFACE_COMPRESSION_EXT, &compressRate);
    ASSERT_GL_NO_ERROR();

    glBindImageTexture(0, tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
    if (nullptr == attribs)
    {
        /* Default attrib which is non-compressed formats will return GL_NO_ERROR. */
        ASSERT_GL_NO_ERROR();
    }
    else if (compressRate == GL_SURFACE_COMPRESSION_FIXED_RATE_DEFAULT_EXT ||
             (compressRate >= GL_SURFACE_COMPRESSION_FIXED_RATE_1BPC_EXT &&
              compressRate <= GL_SURFACE_COMPRESSION_FIXED_RATE_12BPC_EXT))
    {
        /* Compressed texture is not supported in glBindImageTexture. */
        ASSERT_GL_ERROR(GL_INVALID_VALUE);
    }
    else if (attribs[1] == GL_SURFACE_COMPRESSION_FIXED_RATE_NONE_EXT || attribs[0] == GL_NONE)
    {
        /* Default attrib which is non-compressed formats will return GL_NO_ERROR. */
        ASSERT_GL_NO_ERROR();
    }
}

void TextureFixedRateCompressionTest::basicTestHelper(const GLint *attribs)
{
    GLTexture texture;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ASSERT_GL_NO_ERROR();

    glTexStorageAttribs2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16, attribs);
    ASSERT_GL_NO_ERROR();

    /* Query and check the compression rate */
    GLint compressRate;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_SURFACE_COMPRESSION_EXT, &compressRate);
    ASSERT_GL_NO_ERROR();

    if (nullptr != attribs && compressRate != GL_SURFACE_COMPRESSION_FIXED_RATE_NONE_EXT &&
        attribs[1] != GL_SURFACE_COMPRESSION_FIXED_RATE_DEFAULT_EXT)
    {
        EXPECT_EQ(compressRate, attribs[1]);
    }

    GLFramebuffer fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    ASSERT_GL_NO_ERROR();
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    ASSERT_GL_NO_ERROR();
    ASSERT_GL_FRAMEBUFFER_COMPLETE(GL_FRAMEBUFFER);

    ANGLE_GL_PROGRAM(drawRed, essl1_shaders::vs::Simple(), essl1_shaders::fs::Red());
    drawQuad(drawRed, essl1_shaders::PositionAttrib(), 0);
    EXPECT_PIXEL_RECT_EQ(0, 0, 16, 16, GLColor::red);
    ASSERT_GL_NO_ERROR();
}

// Invalid attrib list, GL_INVALID_VALUE is generated.
TEST_P(TextureFixedRateCompressionTest, Invalidate)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_texture_storage_compression"));

    constexpr GLint kAttribListInvalid[3] = {GL_SURFACE_COMPRESSION_EXT, GL_SURFACE_COMPRESSION_EXT,
                                             GL_NONE};

    GLTexture texture;
    glBindTexture(GL_TEXTURE_2D, texture);
    ASSERT_GL_NO_ERROR();

    glTexStorageAttribs2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16, kAttribListInvalid);
    ASSERT_GL_ERROR(GL_INVALID_VALUE);

    for (const GLint *attribs : kDefaultAttribList)
    {
        invalidTestHelper(attribs);
    }
    invalidTestHelper(nullptr);
}

// Test basic usage of glTexStorageAttribs2DEXT
TEST_P(TextureFixedRateCompressionTest, TexStorageAttribs2DEXT)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_texture_storage_compression"));

    for (const GLint *attribs : kDefaultAttribList)
    {
        basicTestHelper(attribs);
    }
}

void TextureFixedRateCompressionTest::RenderToTexture(const GLenum internalFormat,
                                                      const GLint *attribs)
{
    GLint compressRate = 0;
    ASSERT(attribs != nullptr);

    GLTexture texture;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ASSERT_GL_NO_ERROR();

    glTexStorageAttribs2DEXT(GL_TEXTURE_2D, 1, internalFormat, 16, 16, attribs);
    ASSERT_GL_NO_ERROR();

    GLFramebuffer fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    ASSERT_GL_NO_ERROR();
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    ASSERT_GL_NO_ERROR();
    ASSERT_GL_FRAMEBUFFER_COMPLETE(GL_FRAMEBUFFER);

    // Query and check the compression rate
    glGetTexParameteriv(GL_TEXTURE_2D, GL_SURFACE_COMPRESSION_EXT, &compressRate);
    ASSERT_GL_NO_ERROR();
    ASSERT_EQ(compressRate, attribs[1])
        << "Compression rate of original texture: " << Name(compressRate);

    ANGLE_GL_PROGRAM(drawRed, essl1_shaders::vs::Simple(), essl1_shaders::fs::Red());
    drawQuad(drawRed, essl1_shaders::PositionAttrib(), 0);
    EXPECT_PIXEL_RECT_EQ(0, 0, 16, 16, GLColor::red);
    ASSERT_GL_NO_ERROR();

    // Query and check the compression rate after render
    glGetTexParameteriv(GL_TEXTURE_2D, GL_SURFACE_COMPRESSION_EXT, &compressRate);
    ASSERT_GL_NO_ERROR();
    ASSERT_EQ(compressRate, attribs[1])
        << "Compression rate of texture after render: " << Name(compressRate);
}

// Test render to texture with fixed compression
TEST_P(TextureFixedRateCompressionTest, RenderToTexture)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_texture_storage_compression"));

    static GLint attribList[]          = {GL_SURFACE_COMPRESSION_EXT,
                                          GL_SURFACE_COMPRESSION_FIXED_RATE_1BPC_EXT, GL_NONE};
    GLint numRates                     = 0;
    constexpr GLenum internalFormats[] = {GL_RGBA4, GL_R8, GL_RG8, GL_RGB8, GL_RGBA8};

    for (GLenum internalFormat : internalFormats)
    {
        // Test attrib list generated from internal format query.
        glGetInternalformativ(GL_TEXTURE_2D, internalFormat,
                              GL_NUM_SURFACE_COMPRESSION_FIXED_RATES_EXT, 1, &numRates);
        ASSERT_GL_NO_ERROR();
        if (numRates > 0)
        {
            std::vector<GLint> rates(numRates);
            glGetInternalformativ(GL_TEXTURE_2D, internalFormat, GL_SURFACE_COMPRESSION_EXT,
                                  numRates, rates.data());
            ASSERT_GL_NO_ERROR();

            for (int i = 0; i < numRates; ++i)
            {
                attribList[1] = rates[i];
                RenderToTexture(internalFormat, attribList);
            }
        }
    }
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TextureFixedRateCompressionTest);
// Use this to select which configurations (e.g. which renderer, which GLES major version) these
// tests should be run against.
ANGLE_INSTANTIATE_TEST_ES31_AND(TextureFixedRateCompressionTest);

}  // namespace angle
