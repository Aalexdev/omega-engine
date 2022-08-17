#include "Fovea/Renderer.hpp"
#include "Fovea/core.hpp"
#include "Fovea/SingleTimeCommand.hpp"

#include <stdexcept>

namespace Fovea{
	
	Renderer::~Renderer(){
		freeCommandBuffers();
	}

	void Renderer::initialize(size_t vertexBufferSize){
		recreateSwapChain();
		viewport.x = 0.f;
		viewport.y = 0.f;
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		viewport.width = 1.f;
		viewport.height = 1.f;

		scissor.offset = {0, 0};
		scissor.extent = {1, 1};

		createCommandBuffers();
		createVertexBuffer(vertexBufferSize * 6);
		createIndexBuffer(vertexBufferSize * 4);
	}

	void Renderer::createVertexBuffer(uint32_t count){
		sceneVertexBuffer.alloc(sceneVertexSize * count, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		sceneVertexBuffer.map();
		maxSceneVertexSize = sceneVertexBuffer.getBufferSize();
	}

	void Renderer::setSceneVertexSize(uint32_t size, size_t minOffsetAlignement){
		sceneVertexBuffer.setInstanceProperties(size, minOffsetAlignement);
		sceneVertexSize = size;
		sceneAlignement = sceneVertexBuffer.getAlignmentSize();
	}

	void Renderer::createIndexBuffer(uint32_t count){
		size_t size = count * sizeof(uint32_t);

		Buffer staginBuffer;
		staginBuffer.alloc(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

		staginBuffer.map();

		uint32_t* ptr = reinterpret_cast<uint32_t*>(staginBuffer.getMappedMemory());

		uint32_t offset = 0;
		for (uint32_t i=0; i<count; i+=6){
			ptr[i+0] = offset+0;
			ptr[i+1] = offset+1;
			ptr[i+2] = offset+2;
			ptr[i+3] = offset+2;
			ptr[i+4] = offset+3;
			ptr[i+5] = offset+0;
			offset+=4;
		}

		staginBuffer.flush();
		staginBuffer.unmap();

		VkBufferCopy copy;
		copy.srcOffset = 0;
		copy.dstOffset = 0;
		copy.size = size;

		indexBuffer.alloc(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		SingleTimeCommand::copyBuffer(staginBuffer.getBuffer(), indexBuffer.getBuffer(), copy);
	}

	VkCommandBuffer Renderer::beginFrame(){
		assert(!isFrameStarted && "Can't call beginFrame while already in progress");

		auto result = swapChain->acquireNextImage(&currentImageIndex);
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			recreateSwapChain();
			return nullptr;
		}

		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}

		isFrameStarted = true;

		auto commandBuffer = getCurrentCommandBuffer();
		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("failed to begin recording command buffer!");
		}

		return commandBuffer;
	}

	void Renderer::endFrame(){
		assert(isFrameStarted && "can't call endFrame while frame isn't in progress");
		VkCommandBuffer commandBuffer = getCurrentCommandBuffer();

		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
			throw std::runtime_error("failed to record command buffer");
		
		VkResult result = swapChain->submitCommandBuffer(&commandBuffer, &currentImageIndex);

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || isWindowResized){
			isWindowResized = false;
			recreateSwapChain();
		} else if (result != VK_SUCCESS)
			throw std::runtime_error("failed to submit the command buffer");
		
		isFrameStarted = false;
		currentFrameIndex = (currentFrameIndex + 1) % swapChain->getFrameInFlightCount();
	}

	void Renderer::beginSwapChainRenderPass(VkCommandBuffer commandBuffer){
		assert(isFrameStarted && "Can't call beginSwapChainRenderPass if frame isn't in progress");
		assert(commandBuffer == getCurrentCommandBuffer() && "Can't begin render pass on command buffer from a different frame");
		
		VkRenderPassBeginInfo renderPassBeginInfo{};

		renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassBeginInfo.renderPass = swapChain->getRenderPass();
		renderPassBeginInfo.framebuffer = swapChain->getFrameBuffer(currentImageIndex);

		renderPassBeginInfo.renderArea.offset = {0, 0};
		renderPassBeginInfo.renderArea.extent = swapChain->getExtent();

		std::array<VkClearValue, 2> clearValues{};

		clearValues[0].depthStencil = {1.f, 0};
		clearValues[1].color = clearColor;

		renderPassBeginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
		renderPassBeginInfo.pClearValues = clearValues.data();

		vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		viewport.width = static_cast<float>(swapChain->getExtent().width);
		viewport.height = static_cast<float>(swapChain->getExtent().height);
		scissor.extent = swapChain->getExtent();
		
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
	}

	void Renderer::endSwapChainRenderPass(VkCommandBuffer commandBuffer){
		assert(isFrameStarted && "Can't call endSwapChainRenderPass if frame isn't in progress");
		assert(commandBuffer == getCurrentCommandBuffer() && "Can't end render pass on command buffer from a different frame");
		vkCmdEndRenderPass(commandBuffer);
	}

	void Renderer::setClearColor(float r, float g, float b, float a){
		clearColor.float32[0] = r;
		clearColor.float32[1] = g;
		clearColor.float32[2] = b;
		clearColor.float32[3] = a;
	}

	void Renderer::createCommandBuffers(){
		commandBuffers.resize(swapChain->getFrameInFlightCount());

		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = getInstance().renderCommandPool.getCommandPool();
		allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

		if (vkAllocateCommandBuffers(getInstance().logicalDevice.getDevice(), &allocInfo, commandBuffers.data()) != VK_SUCCESS){
			throw std::runtime_error("failed to create renderer command buffers");
		}
	}

	void Renderer::freeCommandBuffers(){
		vkFreeCommandBuffers(getInstance().logicalDevice.getDevice(), getInstance().renderCommandPool.getCommandPool(), static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
		commandBuffers.clear();
	}

	void Renderer::recreateSwapChain(){
		VkDevice device = getInstance().logicalDevice.getDevice();

		VkExtent2D extent = windowExtent;

		// while(extent.width == 0 || extent.height == 0){
		// 	extent ice.getInstance().getWindow().getExtent();
		// 	glfwWaitEvents();
		// }
		vkDeviceWaitIdle(device);

		if (swapChain == nullptr){
			swapChain = std::make_unique<SwapChain>();
			SwapChainBuilder bd;
			bd.setExtent(extent);
			swapChain->initialize(bd);
		} else {
			std::shared_ptr<SwapChain> oldSwapChain = std::move(swapChain);

			swapChain = std::make_unique<SwapChain>(oldSwapChain);

			// set properties
			SwapChainBuilder builder;
			builder.setExtent(extent);
			builder.setRefreshMode(oldSwapChain->getRefreshMode());
			swapChain->initialize(builder);

			if (!oldSwapChain->compareFormats(*swapChain.get()))
				throw std::runtime_error("swap chain image or depth format has changed");

			
			viewport.width = static_cast<float>(swapChain->getExtent().width);
			viewport.height = static_cast<float>(swapChain->getExtent().height);
			scissor.extent = swapChain->getExtent();
		}
	}

	void Renderer::windowResized(uint32_t width, uint32_t height){
		isWindowResized = true;
		windowExtent = {width, height};
	}

	void Renderer::setScene(void* v, uint32_t vertexCount){
		void* ptr = sceneVertexBuffer.getMappedMemory();

		sceneVertexBufferUsedSize = sceneVertexSize * vertexCount;
		memcpy(ptr, v, sceneVertexBufferUsedSize);
		sceneIndexUsed = (vertexCount / 4) * 6;
		sceneVertexBuffer.flush(sceneAlignement * vertexCount);
	}

	void Renderer::renderScene(VkCommandBuffer commandBuffer){
		if (sceneIndexUsed == 0) return;

		vkCmdBindIndexBuffer(commandBuffer, indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
		VkBuffer vertexBuffer = sceneVertexBuffer.getBuffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);

		vkCmdDrawIndexed(commandBuffer, sceneIndexUsed, 1, 0, 0, 0);
	}

	void Renderer::setSceneData(uint32_t offset, uint32_t count, void* data){
		char* ptr = static_cast<char*>(sceneVertexBuffer.getMappedMemory()) + offset;
		memcpy(ptr, data, sceneAlignement * count);
	} 

	void Renderer::flushSceneData(uint32_t offset, uint32_t count){
		sceneVertexBuffer.flush(sceneAlignement * count, offset);
	}
}