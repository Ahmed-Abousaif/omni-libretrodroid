/*
 *     Copyright (C) 2025  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "immersivemode.h"

#include <sstream>
#include <vector>

#include "log.h"

namespace libretrodroid {

static bool checkCompileStatus(GLuint shader, const char* name) {
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 0) {
            std::vector<char> infoLog(infoLen);
            glGetShaderInfoLog(shader, infoLen, nullptr, infoLog.data());
            LOGE("ImmersiveMode shader '%s' compilation error: %s", name, infoLog.data());
        }
        return false;
    }
    return true;
}

static bool checkLinkStatus(GLuint program, const char* name) {
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint infoLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 0) {
            std::vector<char> infoLog(infoLen);
            glGetProgramInfoLog(program, infoLen, nullptr, infoLog.data());
            LOGE("ImmersiveMode program '%s' link error: %s", name, infoLog.data());
        }
        return false;
    }
    return true;
}

ImmersiveMode::~ImmersiveMode() {
    if (vao != 0) {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }
    if (vboPosition != 0) {
        glDeleteBuffers(1, &vboPosition);
        vboPosition = 0;
    }
    if (vboTexCoord != 0) {
        glDeleteBuffers(1, &vboTexCoord);
        vboTexCoord = 0;
    }
    if (blendShaderProgram != 0) {
        glDeleteProgram(blendShaderProgram);
        blendShaderProgram = 0;
    }
    if (blurShaderProgram != 0) {
        glDeleteProgram(blurShaderProgram);
        blurShaderProgram = 0;
    }
    if (displayShaderProgram != 0) {
        glDeleteProgram(displayShaderProgram);
        displayShaderProgram = 0;
    }
    for (auto& fb : blurFramebuffers) {
        ES3Utils::deleteFramebuffer(std::move(fb));
    }
    blurFramebuffers.clear();
}

void ImmersiveMode::initializeShaders() {
    if (blendShaderProgram != 0) return;

    GLuint blendingVertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(blendingVertexShader, 1, &blendingVertexShaderSource, nullptr);
    glCompileShader(blendingVertexShader);
    checkCompileStatus(blendingVertexShader, "blendingVertexShader");

    GLuint blendingFragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(blendingFragmentShader, 1, &blendingFragmentShaderSource, nullptr);
    glCompileShader(blendingFragmentShader);
    checkCompileStatus(blendingFragmentShader, "blendingFragmentShader");

    blendShaderProgram = glCreateProgram();
    glAttachShader(blendShaderProgram, blendingVertexShader);
    glAttachShader(blendShaderProgram, blendingFragmentShader);
    glBindAttribLocation(blendShaderProgram, 0, "aPosition");
    glBindAttribLocation(blendShaderProgram, 1, "aTexCoord");
    glLinkProgram(blendShaderProgram);
    checkLinkStatus(blendShaderProgram, "blendShaderProgram");

    glDeleteShader(blendingVertexShader);
    glDeleteShader(blendingFragmentShader);

    blendPositionHandle = glGetAttribLocation(blendShaderProgram, "aPosition");
    blendTextureCoordinatesHandle = glGetAttribLocation(blendShaderProgram, "aTexCoord");
    blendTextureHandle = glGetUniformLocation(blendShaderProgram, "currentFrame");
    blendPrevTextureHandle = glGetUniformLocation(blendShaderProgram, "previousFrame");
    blendFactorHandle = glGetUniformLocation(blendShaderProgram, "blendFactor");

    std::string fragmentShaderSource = generateBlurShader();

    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &defaultVertexShaderSource, nullptr);
    glCompileShader(vertexShader);
    checkCompileStatus(vertexShader, "blurVertexShader");

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    const char* c_str = fragmentShaderSource.c_str();
    glShaderSource(fragmentShader, 1, &c_str, nullptr);
    glCompileShader(fragmentShader);
    checkCompileStatus(fragmentShader, "blurFragmentShader");

    blurShaderProgram = glCreateProgram();
    glAttachShader(blurShaderProgram, vertexShader);
    glAttachShader(blurShaderProgram, fragmentShader);
    glBindAttribLocation(blurShaderProgram, 0, "aPosition");
    glBindAttribLocation(blurShaderProgram, 1, "aTexCoord");
    glLinkProgram(blurShaderProgram);
    checkLinkStatus(blurShaderProgram, "blurShaderProgram");

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    blurPositionHandle = glGetAttribLocation(blurShaderProgram, "aPosition");
    blurTextureCoordinatesHandle = glGetAttribLocation(blurShaderProgram, "aTexCoord");
    blurTextureHandle = glGetUniformLocation(blurShaderProgram, "texture");
    blurDirectionHandle = glGetUniformLocation(blurShaderProgram, "direction");

    GLuint displayVertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(displayVertexShader, 1, &defaultVertexShaderSource, nullptr);
    glCompileShader(displayVertexShader);
    checkCompileStatus(displayVertexShader, "displayVertexShader");

    GLuint displayFragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(displayFragmentShader, 1, &displayFragmentShaderSource, nullptr);
    glCompileShader(displayFragmentShader);
    checkCompileStatus(displayFragmentShader, "displayFragmentShader");

    displayShaderProgram = glCreateProgram();
    glAttachShader(displayShaderProgram, displayVertexShader);
    glAttachShader(displayShaderProgram, displayFragmentShader);
    glBindAttribLocation(displayShaderProgram, 0, "aPosition");
    glBindAttribLocation(displayShaderProgram, 1, "aTexCoord");
    glLinkProgram(displayShaderProgram);
    checkLinkStatus(displayShaderProgram, "displayShaderProgram");

    glDeleteShader(displayVertexShader);
    glDeleteShader(displayFragmentShader);

    displayPositionHandle = glGetAttribLocation(displayShaderProgram, "aPosition");
    displayForegroundBoundsHandle = glGetUniformLocation(displayShaderProgram, "foregroundBounds");
    displayTextureCoordinatesHandle = glGetAttribLocation(displayShaderProgram, "aTexCoord");
    displayTextureHandle = glGetUniformLocation(displayShaderProgram, "texture");

    if (vao == 0) {
        glGenVertexArrays(1, &vao);
    }
    if (vboPosition == 0) {
        glGenBuffers(1, &vboPosition);
    }
    if (vboTexCoord == 0) {
        glGenBuffers(1, &vboTexCoord);
        glBindBuffer(GL_ARRAY_BUFFER, vboTexCoord);
        glBufferData(GL_ARRAY_BUFFER, sizeof(gTextureCoords), gTextureCoords, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

void ImmersiveMode::initializeFramebuffers() {
    if (!blurFramebuffers.empty()) return;

    for (int i = 0; i < 3; i++) {
        blurFramebuffers.push_back(
            ES3Utils::createFramebuffer(
                downscaledWidth, downscaledHeight, true, false, false, false
            )
        );
    }

    // Last framebuffer needs GL_MIRROR_REPEAT
    blurFramebuffers.push_back(
        ES3Utils::createFramebuffer(
            downscaledWidth, downscaledHeight, true, true, false, false
        )
    );
}

void ImmersiveMode::renderToFramebuffer(uintptr_t texture, GLfloat* gBackgroundVertices) {
    if (blurFramebuffers.size() < 4) return;
    int blendFramebufferReadIndex = (blendFramebufferWriteIndex + 1) % 2;

    glViewport(0, 0, downscaledWidth, downscaledHeight);

    if (vao != 0) {
        glBindVertexArray(vao);
    }

    // Upload vertices to vboPosition
    glBindBuffer(GL_ARRAY_BUFFER, vboPosition);
    glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(float), gBackgroundVertices, GL_STREAM_DRAW);
    glVertexAttribPointer(blendPositionHandle, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(blendPositionHandle);

    // Bind vboTexCoord
    glBindBuffer(GL_ARRAY_BUFFER, vboTexCoord);
    glVertexAttribPointer(blendTextureCoordinatesHandle, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(blendTextureCoordinatesHandle);

    glBindFramebuffer(GL_FRAMEBUFFER, blurFramebuffers[blendFramebufferWriteIndex]->framebuffer);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(blendShaderProgram);
    glUniform1f(blendFactorHandle, blendFactor);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(blendTextureHandle, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurFramebuffers[blendFramebufferReadIndex]->texture);
    glUniform1i(blendPrevTextureHandle, 1);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glUseProgram(blurShaderProgram);

    if (blurPositionHandle != blendPositionHandle) {
        glDisableVertexAttribArray(blendPositionHandle);
        glBindBuffer(GL_ARRAY_BUFFER, vboPosition);
        glVertexAttribPointer(blurPositionHandle, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
        glEnableVertexAttribArray(blurPositionHandle);
    }
    if (blurTextureCoordinatesHandle != blendTextureCoordinatesHandle) {
        glDisableVertexAttribArray(blendTextureCoordinatesHandle);
        glBindBuffer(GL_ARRAY_BUFFER, vboTexCoord);
        glVertexAttribPointer(blurTextureCoordinatesHandle, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
        glEnableVertexAttribArray(blurTextureCoordinatesHandle);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, blurFramebuffers[2]->framebuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurFramebuffers[blendFramebufferWriteIndex]->texture);
    glUniform1i(blurTextureHandle, 0);
    glUniform2f(blurDirectionHandle, 1.0f / downscaledWidth, 0.0f);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindFramebuffer(GL_FRAMEBUFFER, blurFramebuffers[3]->framebuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurFramebuffers[2]->texture);
    glUniform1i(blurTextureHandle, 0);
    glUniform2f(blurDirectionHandle, 0.0f, 1.0f / downscaledHeight);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(blurPositionHandle);
    glDisableVertexAttribArray(blurTextureCoordinatesHandle);
    if (blendPositionHandle != blurPositionHandle) {
        glDisableVertexAttribArray(blendPositionHandle);
    }
    if (blendTextureCoordinatesHandle != blurTextureCoordinatesHandle) {
        glDisableVertexAttribArray(blendTextureCoordinatesHandle);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (vao != 0) {
        glBindVertexArray(0);
    }

    blendFramebufferWriteIndex = blendFramebufferReadIndex;
}

void ImmersiveMode::renderToFinalOutput(
    unsigned screenWidth,
    unsigned screenHeight,
    std::array<float, 12> backgroundVertices,
    std::array<float, 4> foregroundBounds
) {
    if (blurFramebuffers.size() < 4) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenWidth, screenHeight);
    glUseProgram(displayShaderProgram);

    if (vao != 0) {
        glBindVertexArray(vao);
    }

    // Upload background vertices to vboPosition
    glBindBuffer(GL_ARRAY_BUFFER, vboPosition);
    glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(float), backgroundVertices.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(displayPositionHandle, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(displayPositionHandle);

    // Bind vboTexCoord
    glBindBuffer(GL_ARRAY_BUFFER, vboTexCoord);
    glVertexAttribPointer(displayTextureCoordinatesHandle, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(displayTextureCoordinatesHandle);

    glUniform4f(
        displayForegroundBoundsHandle,
        foregroundBounds[0],
        foregroundBounds[1],
        foregroundBounds[2],
        foregroundBounds[3]
    );

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurFramebuffers[3]->texture);
    glUniform1i(displayTextureHandle, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(displayPositionHandle);
    glDisableVertexAttribArray(displayTextureCoordinatesHandle);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    if (vao != 0) {
        glBindVertexArray(0);
    }
}

void ImmersiveMode::renderBackground(
    unsigned screenWidth,
    unsigned screenHeight,
    std::array<float, 12> backgroundVertices,
    std::array<float, 4> foregroundBounds,
    GLfloat* framebufferVertices,
    uintptr_t texture
) {
    if (texture == 0) return;

    initializeShaders();
    initializeFramebuffers();

    if (blendFramebufferCurrent == 0) {
        renderToFramebuffer(texture, framebufferVertices);
    }

    // Let's update the blurred texture every other frame.
    blendFramebufferCurrent = (blendFramebufferCurrent + 1) % blurSkipUpdate;

    renderToFinalOutput(screenWidth, screenHeight, backgroundVertices, foregroundBounds);
}

std::vector<float> ImmersiveMode::generateSmoothingWeights(int size, float brightness) {
    std::vector<float> kernel(size);
    float sigma = size / 3.0f;
    float sum = 0.0f;
    int halfSize = size / 2;

    for (int i = 0; i < size; i++) {
        int x = i - halfSize;
        kernel[i] = exp(-(x * x) / (2 * sigma * sigma));
        sum += kernel[i];
    }

    for (int i = 0; i < size; i++) {
        kernel[i] = (kernel[i] / sum) * brightness;
    }

    return kernel;
}

std::string ImmersiveMode::generateBlurShader() {
    if (blurMaskSize % 2 == 0) {
        LOGE("Error: maskSize should be an odd number!");
        return "";
    }

    std::vector<float> kernel = generateSmoothingWeights(blurMaskSize, sqrt(blurBrightness));

    std::ostringstream result;
    result << R"(
        precision mediump float;
        varying vec2 vTexCoord;
        uniform sampler2D texture;
        uniform vec2 direction;

        void main() {
            lowp vec4 result = vec4(0.0);
            lowp float kernel[)" << blurMaskSize << R"(];

    )";

    for (int i = 0; i < blurMaskSize; i++) {
        result << "            kernel[" << i << "] = " << kernel[i] << ";\n";
    }

    int halfMask = blurMaskSize / 2;
    result << R"(
            for (int i = 0; i < )" << blurMaskSize << R"(; i++) {
                lowp vec2 offset = vec2(float(i - )" << halfMask << R"()) * direction;
                result += texture2D(texture, vTexCoord + offset) * kernel[i];
            }
            gl_FragColor = result;
        }
    )";

    return result.str();
}

} // namespace libretrodroid
