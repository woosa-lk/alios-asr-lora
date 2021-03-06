/*
 * Copyright (c) 2017, NXP Semiconductors, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "fsl_spi_dma.h"

/*******************************************************************************
 * Definitons
 ******************************************************************************/
/*<! Structure definition for spi_dma_private_handle_t. The structure is private. */
typedef struct _spi_dma_private_handle
{
    SPI_Type *base;
    spi_dma_handle_t *handle;
} spi_dma_private_handle_t;

/*! @brief SPI transfer state, which is used for SPI transactiaonl APIs' internal state. */
enum _spi_dma_states_t
{
    kSPI_Idle = 0x0, /*!< SPI is idle state */
    kSPI_Busy        /*!< SPI is busy tranferring data. */
};

typedef struct _spi_dma_txdummy
{
    uint32_t lastWord;
    uint32_t word;
} spi_dma_txdummy_t;

/*<! Private handle only used for internally. */
static spi_dma_private_handle_t s_dmaPrivateHandle[FSL_FEATURE_SOC_SPI_COUNT];
/*******************************************************************************
 * Prototypes
 ******************************************************************************/

/*!
* @brief SPI private function to return SPI configuration
*
* @param base SPI base address.
*/
void *SPI_GetConfig(SPI_Type *base);

/*!
 * @brief DMA callback function for SPI send transfer.
 *
 * @param handle DMA handle pointer.
 * @param userData User data for DMA callback function.
 */
static void SPI_TxDMACallback(dma_handle_t *handle, void *userData, bool transferDone, uint32_t intmode);

/*!
 * @brief DMA callback function for SPI receive transfer.
 *
 * @param handle DMA handle pointer.
 * @param userData User data for DMA callback function.
 */
static void SPI_RxDMACallback(dma_handle_t *handle, void *userData, bool transferDone, uint32_t intmode);

/*******************************************************************************
 * Variables
 ******************************************************************************/
#if defined(__ICCARM__)
#pragma data_alignment = 4
static spi_dma_txdummy_t s_txDummy[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#elif defined(__CC_ARM)
__attribute__((aligned(4))) static spi_dma_txdummy_t s_txDummy[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#elif defined(__GNUC__)
__attribute__((aligned(4))) static spi_dma_txdummy_t s_txDummy[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#endif

#if defined(__ICCARM__)
#pragma data_alignment = 4
static uint16_t s_rxDummy;
static uint32_t s_txLastData[FSL_FEATURE_SOC_SPI_COUNT];
#elif defined(__CC_ARM)
__attribute__((aligned(4))) static uint16_t s_rxDummy;
__attribute__((aligned(4))) static uint32_t s_txLastData[FSL_FEATURE_SOC_SPI_COUNT];
#elif defined(__GNUC__)
__attribute__((aligned(4))) static uint16_t s_rxDummy;
__attribute__((aligned(4))) static uint32_t s_txLastData[FSL_FEATURE_SOC_SPI_COUNT];
#endif

#if defined(__ICCARM__)
#pragma data_alignment = 16
static dma_descriptor_t s_spi_descriptor_table[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#elif defined(__CC_ARM)
__attribute__((aligned(16))) static dma_descriptor_t s_spi_descriptor_table[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#elif defined(__GNUC__)
__attribute__((aligned(16))) static dma_descriptor_t s_spi_descriptor_table[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#endif

/*! @brief Global variable for dummy data value setting. */
extern volatile uint8_t s_dummyData[];
/*******************************************************************************
* Code
******************************************************************************/

static void XferToFifoWR(spi_transfer_t *xfer, uint32_t *fifowr)
{
    *fifowr |= (xfer->configFlags & (uint32_t)kSPI_FrameDelay) ? (uint32_t)kSPI_FrameDelay : 0;
    *fifowr |= (xfer->configFlags & (uint32_t)kSPI_FrameAssert) ? (uint32_t)kSPI_FrameAssert : 0;
    *fifowr |= (xfer->configFlags & (uint32_t)kSPI_ReceiveIgnore) ? (uint32_t)kSPI_ReceiveIgnore : 0;
}

static void SpiConfigToFifoWR(spi_config_t *config, uint32_t *fifowr)
{
    *fifowr |= (SPI_DEASSERT_ALL & (~SPI_DEASSERT_SSELNUM(config->sselNum)));
    /* set width of data - range asserted at entry */
    *fifowr |= SPI_TXDATCTL_LEN(config->dataWidth);
}

static void SPI_SetupDummy(SPI_Type *base, spi_dma_txdummy_t *dummy, spi_transfer_t *xfer, spi_config_t *spi_config_p)
{
    uint32_t instance = SPI_GetInstance(base);
    dummy->word = ((uint32_t)s_dummyData[instance] << 8U) | s_dummyData[instance];
    XferToFifoWR(xfer, &dummy->word);
    SpiConfigToFifoWR(spi_config_p, &dummy->word);
    if ((xfer->configFlags & kSPI_FrameAssert) &&
        ((spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2) : (xfer->dataSize > 1)))
    {
        dummy->lastWord = ((uint32_t)s_dummyData[instance] << 8U) | s_dummyData[instance];
        XferToFifoWR(xfer, &dummy->lastWord);
        SpiConfigToFifoWR(spi_config_p, &dummy->lastWord);
        dummy->word &= (uint32_t)(~kSPI_FrameAssert);
    }
}

status_t SPI_MasterTransferCreateHandleDMA(SPI_Type *base,
                                           spi_dma_handle_t *handle,
                                           spi_dma_callback_t callback,
                                           void *userData,
                                           dma_handle_t *txHandle,
                                           dma_handle_t *rxHandle)
{
    int32_t instance = 0;

    /* check 'base' */
    assert(!(NULL == base));
    if (NULL == base)
    {
        return kStatus_InvalidArgument;
    }
    /* check 'handle' */
    assert(!(NULL == handle));
    if (NULL == handle)
    {
        return kStatus_InvalidArgument;
    }

    instance = SPI_GetInstance(base);

    memset(handle, 0, sizeof(*handle));
    /* Set spi base to handle */
    handle->txHandle = txHandle;
    handle->rxHandle = rxHandle;
    handle->callback = callback;
    handle->userData = userData;

    /* Set SPI state to idle */
    handle->state = kSPI_Idle;

    /* Set handle to global state */
    s_dmaPrivateHandle[instance].base = base;
    s_dmaPrivateHandle[instance].handle = handle;

    /* Install callback for Tx dma channel */
    DMA_SetCallback(handle->txHandle, SPI_TxDMACallback, &s_dmaPrivateHandle[instance]);
    DMA_SetCallback(handle->rxHandle, SPI_RxDMACallback, &s_dmaPrivateHandle[instance]);

    return kStatus_Success;
}

status_t SPI_MasterTransferDMA(SPI_Type *base, spi_dma_handle_t *handle, spi_transfer_t *xfer)
{
    int32_t instance;
    status_t result = kStatus_Success;
    spi_config_t *spi_config_p;

    assert(!((NULL == handle) || (NULL == xfer)));
    if ((NULL == handle) || (NULL == xfer))
    {
        return kStatus_InvalidArgument;
    }
    /* byte size is zero. */
    assert(!(xfer->dataSize == 0));
    if (xfer->dataSize == 0)
    {
        return kStatus_InvalidArgument;
    }

    /* cannot get instance from base address */
    instance = SPI_GetInstance(base);
    assert(!(instance < 0));
    if (instance < 0)
    {
        return kStatus_InvalidArgument;
    }

    /* Check if the device is busy */
    if (handle->state == kSPI_Busy)
    {
        return kStatus_SPI_Busy;
    }
    else
    {
        uint32_t tmp;
        dma_transfer_config_t xferConfig = {0};
        spi_config_p = (spi_config_t *)SPI_GetConfig(base);

        handle->state = kStatus_SPI_Busy;
        handle->transferSize = xfer->dataSize;

        /* Receive */
        if (xfer->rxData)
        {
            if (SPI_IsRxFifoEnabled(base))
            {
                DMA_PrepareTransfer(&xferConfig, (void *)&VFIFO->SPI[instance].RXDATSPI, xfer->rxData,
                                    ((spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                    xfer->dataSize, kDMA_PeripheralToMemory, NULL);
            }
            else
            {
                DMA_PrepareTransfer(&xferConfig, (void *)&base->RXDAT, xfer->rxData,
                                    ((spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                    xfer->dataSize, kDMA_PeripheralToMemory, NULL);
            }
        }
        else
        {
            if (SPI_IsRxFifoEnabled(base))
            {
                DMA_PrepareTransfer(&xferConfig, (void *)&VFIFO->SPI[instance].RXDATSPI, &s_rxDummy,
                                    ((spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                    xfer->dataSize, kDMA_StaticToStatic, NULL);
            }
            else
            {
                DMA_PrepareTransfer(&xferConfig, (void *)&base->RXDAT, &s_rxDummy,
                                    ((spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                    xfer->dataSize, kDMA_StaticToStatic, NULL);
            }
        }
        DMA_SubmitTransfer(handle->rxHandle, &xferConfig);
        handle->rxInProgress = true;
        DMA_StartTransfer(handle->rxHandle);

        /* Transmit */
        tmp = 0;
        XferToFifoWR(xfer, &tmp);
        SpiConfigToFifoWR(spi_config_p, &tmp);

        if ((xfer->configFlags & kSPI_FrameAssert) &&
            ((spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2) : (xfer->dataSize > 1)))
        {
            if (spi_config_p->dataWidth > 7U)
            {
                s_txLastData[instance] =
                    tmp | ((uint32_t)(xfer->txData[xfer->dataSize - 1]) << 8U) | (xfer->txData[xfer->dataSize - 2]);
            }
            else
            {
                s_txLastData[instance] = tmp | (xfer->txData[xfer->dataSize - 1]);
            }

            /* If not the last data, clear the end of transfer control bit. */
            tmp &= ~((uint32_t)(kSPI_FrameAssert));
        }

        if (xfer->txData)
        {
            if (SPI_IsTxFifoEnabled(base))
            {
                if ((xfer->configFlags & kSPI_FrameAssert) &&
                    ((spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2) : (xfer->dataSize > 1)))
                {
                    dma_xfercfg_t tmp_xfercfg = {0};
                    tmp_xfercfg.valid = true;
                    tmp_xfercfg.swtrig = true;
                    tmp_xfercfg.intA = true;
                    tmp_xfercfg.byteWidth = sizeof(uint32_t);
                    tmp_xfercfg.srcInc = 0;
                    tmp_xfercfg.dstInc = 0;
                    tmp_xfercfg.transferCount = 1;
                    /* create chained descriptor to transmit last word */
                    DMA_CreateDescriptor(&s_spi_descriptor_table[instance], &tmp_xfercfg, &s_txLastData[instance],
                                         (uint32_t *)&VFIFO->SPI[instance].TXDATSPI, NULL);
                    if (spi_config_p->dataWidth > 7U)
                    {
                        DMA_PrepareTransfer(&xferConfig, (xfer->txData), (void *)&VFIFO->SPI[instance].TXDATSPI,
                                            sizeof(uint16_t), (xfer->dataSize - 2), kDMA_MemoryToPeripheral,
                                            &s_spi_descriptor_table[instance]);
                    }
                    else
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (void *)&VFIFO->SPI[instance].TXDATSPI,
                                            sizeof(uint8_t), (xfer->dataSize - 1), kDMA_MemoryToPeripheral,
                                            &s_spi_descriptor_table[instance]);
                    }

                    /* Disable interrupts for first descriptor to avoid calling callback twice */
                    xferConfig.xfercfg.intA = false;
                    xferConfig.xfercfg.intB = false;
                }
                else
                {
                    if (spi_config_p->dataWidth > 7U)
                    {
                        DMA_PrepareTransfer(&xferConfig, (xfer->txData), (void *)&VFIFO->SPI[instance].TXDATSPI,
                                            sizeof(uint16_t), (xfer->dataSize), kDMA_MemoryToPeripheral, NULL);
                    }
                    else
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (void *)&VFIFO->SPI[instance].TXDATSPI,
                                            sizeof(uint8_t), (xfer->dataSize), kDMA_MemoryToPeripheral, NULL);
                    }
                }
            }
            else
            {
                if ((xfer->configFlags & kSPI_FrameAssert) &&
                    ((spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2) : (xfer->dataSize > 1)))
                {
                    dma_xfercfg_t tmp_xfercfg = {0};
                    tmp_xfercfg.valid = true;
                    tmp_xfercfg.swtrig = true;
                    tmp_xfercfg.intA = true;
                    tmp_xfercfg.byteWidth = sizeof(uint32_t);
                    tmp_xfercfg.srcInc = 0;
                    tmp_xfercfg.dstInc = 0;
                    tmp_xfercfg.transferCount = 1;
                    /* Create chained descriptor to transmit last word */
                    DMA_CreateDescriptor(&s_spi_descriptor_table[instance], &tmp_xfercfg, &s_txLastData[instance],
                                         (uint32_t *)&base->TXDATCTL, NULL);
                    if (spi_config_p->dataWidth > 7U)
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (void *)&base->TXDAT, sizeof(uint16_t),
                                            (xfer->dataSize - 2), kDMA_MemoryToPeripheral,
                                            &s_spi_descriptor_table[instance]);
                    }
                    else
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (void *)&base->TXDAT, sizeof(uint8_t),
                                            (xfer->dataSize - 1), kDMA_MemoryToPeripheral,
                                            &s_spi_descriptor_table[instance]);
                    }
                    /* disable interrupts for first descriptor to avoid calling callback twice */
                    xferConfig.xfercfg.intA = false;
                    xferConfig.xfercfg.intB = false;
                }
                else
                {
                    if (spi_config_p->dataWidth > 7U)
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (void *)&base->TXDAT, sizeof(uint16_t),
                                            (xfer->dataSize), kDMA_MemoryToPeripheral, NULL);
                    }
                    else
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (void *)&base->TXDAT, sizeof(uint8_t),
                                            (xfer->dataSize), kDMA_MemoryToPeripheral, NULL);
                    }
                }
            }
            result = DMA_SubmitTransfer(handle->txHandle, &xferConfig);
            if (result != kStatus_Success)
            {
                return result;
            }
        }
        else
        {
            /* Create chained descriptor to transmit dummy word. */
            SPI_SetupDummy(base, &s_txDummy[instance], xfer, spi_config_p);

            if ((xfer->configFlags & kSPI_FrameAssert) &&
                ((spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2) : (xfer->dataSize > 1)))
            {
                dma_xfercfg_t tmp_xfercfg = {0};
                tmp_xfercfg.valid = true;
                tmp_xfercfg.swtrig = true;
                tmp_xfercfg.intA = true;
                tmp_xfercfg.byteWidth = sizeof(uint32_t);
                tmp_xfercfg.srcInc = 0;
                tmp_xfercfg.dstInc = 0;
                tmp_xfercfg.transferCount = 1;

                if (SPI_IsTxFifoEnabled(base))
                {
                    DMA_CreateDescriptor(&s_spi_descriptor_table[instance], &tmp_xfercfg, &s_txDummy[instance].lastWord,
                                         (uint32_t *)&VFIFO->SPI[instance].TXDATSPI, NULL);
                }
                else
                {
                    DMA_CreateDescriptor(&s_spi_descriptor_table[instance], &tmp_xfercfg, &s_txDummy[instance].lastWord,
                                         (uint32_t *)&base->TXDATCTL, NULL);
                }

                if (SPI_IsTxFifoEnabled(base))
                {
                    DMA_PrepareTransfer(&xferConfig, &s_txDummy[instance].word, (void *)&VFIFO->SPI[instance].TXDATSPI,
                                        ((spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                        ((spi_config_p->dataWidth > 7U) ? (xfer->dataSize - 2) : (xfer->dataSize - 1)),
                                        kDMA_StaticToStatic, &s_spi_descriptor_table[instance]);
                }
                else
                {
                    DMA_PrepareTransfer(&xferConfig, &s_txDummy[instance].word, (void *)&base->TXDATCTL,
                                        ((spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                        ((spi_config_p->dataWidth > 7U) ? (xfer->dataSize - 2) : (xfer->dataSize - 1)),
                                        kDMA_StaticToStatic, &s_spi_descriptor_table[instance]);
                }
                /* Disable interrupts for first descriptor to avoid calling callback twice */
                xferConfig.xfercfg.intA = false;
                xferConfig.xfercfg.intB = false;
                result = DMA_SubmitTransfer(handle->txHandle, &xferConfig);
                if (result != kStatus_Success)
                {
                    return result;
                }
            }
            else
            {
                if (SPI_IsTxFifoEnabled(base))
                {
                    DMA_PrepareTransfer(&xferConfig, &s_txDummy[instance].word, (void *)&VFIFO->SPI[instance].TXDATSPI,
                                        ((spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                        (xfer->dataSize), kDMA_StaticToStatic, NULL);
                }
                else
                {
                    DMA_PrepareTransfer(&xferConfig, &s_txDummy[instance].word, (void *)&base->TXDAT,
                                        ((spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                        (xfer->dataSize), kDMA_StaticToStatic, NULL);
                }
                result = DMA_SubmitTransfer(handle->txHandle, &xferConfig);
                if (result != kStatus_Success)
                {
                    return result;
                }
            }
        }
        handle->txInProgress = true;
        /* Setup the control information. */
        if (SPI_IsTxFifoEnabled(base))
        {
            *((uint16_t *)&(VFIFO->SPI[instance].TXDATSPI) + 1) = (tmp >> 16U);
        }
        else
        {
            base->TXCTL = tmp;
        }
        DMA_StartTransfer(handle->txHandle);
    }

    return result;
}

status_t SPI_MasterHalfDuplexTransferDMA(SPI_Type *base, spi_dma_handle_t *handle, spi_half_duplex_transfer_t *xfer)
{
    assert(xfer);
    assert(handle);
    spi_transfer_t tempXfer = {0};
    status_t status;

    if (xfer->isTransmitFirst)
    {
        tempXfer.txData = xfer->txData;
        tempXfer.rxData = NULL;
        tempXfer.dataSize = xfer->txDataSize;
    }
    else
    {
        tempXfer.txData = NULL;
        tempXfer.rxData = xfer->rxData;
        tempXfer.dataSize = xfer->rxDataSize;
    }
    /* If the pcs pin keep assert between transmit and receive. */
    if (xfer->isPcsAssertInTransfer)
    {
        tempXfer.configFlags = (xfer->configFlags) & (uint32_t)(~kSPI_FrameAssert);
    }
    else
    {
        tempXfer.configFlags = (xfer->configFlags) | kSPI_FrameAssert;
    }

    status = SPI_MasterTransferBlocking(base, &tempXfer);
    if (status != kStatus_Success)
    {
        return status;
    }

    if (xfer->isTransmitFirst)
    {
        tempXfer.txData = NULL;
        tempXfer.rxData = xfer->rxData;
        tempXfer.dataSize = xfer->rxDataSize;
    }
    else
    {
        tempXfer.txData = xfer->txData;
        tempXfer.rxData = NULL;
        tempXfer.dataSize = xfer->txDataSize;
    }
    tempXfer.configFlags = xfer->configFlags;

    status = SPI_MasterTransferDMA(base, handle, &tempXfer);

    return status;
}

static void SPI_RxDMACallback(dma_handle_t *handle, void *userData, bool transferDone, uint32_t intmode)
{
    spi_dma_private_handle_t *privHandle = (spi_dma_private_handle_t *)userData;
    spi_dma_handle_t *spiHandle = privHandle->handle;
    SPI_Type *base = privHandle->base;

    /* change the state */
    spiHandle->rxInProgress = false;

    /* All finished, call the callback */
    if ((spiHandle->txInProgress == false) && (spiHandle->rxInProgress == false))
    {
        spiHandle->state = kSPI_Idle;
        if (spiHandle->callback)
        {
            (spiHandle->callback)(base, spiHandle, kStatus_Success, spiHandle->userData);
        }
    }
}

static void SPI_TxDMACallback(dma_handle_t *handle, void *userData, bool transferDone, uint32_t intmode)
{
    spi_dma_private_handle_t *privHandle = (spi_dma_private_handle_t *)userData;
    spi_dma_handle_t *spiHandle = privHandle->handle;
    SPI_Type *base = privHandle->base;
    /* change the state */
    spiHandle->txInProgress = false;
    /* All finished, call the callback */
    if ((spiHandle->txInProgress == false) && (spiHandle->rxInProgress == false))
    {
        spiHandle->state = kSPI_Idle;
        if (spiHandle->callback)
        {
            (spiHandle->callback)(base, spiHandle, kStatus_Success, spiHandle->userData);
        }
    }
}

void SPI_MasterTransferAbortDMA(SPI_Type *base, spi_dma_handle_t *handle)
{
    assert(NULL != handle);

    /* Stop tx transfer first */
    DMA_AbortTransfer(handle->txHandle);
    /* Then rx transfer */
    DMA_AbortTransfer(handle->rxHandle);

    /* Set the handle state */
    handle->txInProgress = false;
    handle->rxInProgress = false;
    handle->state = kSPI_Idle;
}

status_t SPI_MasterTransferGetCountDMA(SPI_Type *base, spi_dma_handle_t *handle, size_t *count)
{
    assert(handle);

    if (!count)
    {
        return kStatus_InvalidArgument;
    }

    /* Catch when there is not an active transfer. */
    if (handle->state != kSPI_Busy)
    {
        *count = 0;
        return kStatus_NoTransferInProgress;
    }

    size_t bytes;

    bytes = DMA_GetRemainingBytes(handle->rxHandle->base, handle->rxHandle->channel);

    *count = handle->transferSize - bytes;

    return kStatus_Success;
}
