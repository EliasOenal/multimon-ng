#include <Windows.h>
#include <stdio.h>

#define BUFFER_LEN_IN_MS 20 
#define BUFFERS 50

int SAMPLES_PER_BUFFER;
WAVEFORMATEX g_WavFmt = {0};
HWAVEIN hWavIn;

void process_buffer(float *buf, unsigned int len);
int Overlap;
float *fbuf;

void CALLBACK waveInProc(HWAVEIN hwi,UINT uMsg,DWORD dwInstance,DWORD dwParam1,DWORD dwParam2)
{
	WAVEHDR* pWaveHdr;
	SHORT *sp;
	switch(uMsg)
	{
	case MM_WIM_DATA:
		pWaveHdr = ((WAVEHDR*)dwParam1 );
		sp = (SHORT*)pWaveHdr->lpData;
                int i;
                for (i=0; i<SAMPLES_PER_BUFFER; i++)
		{
			fbuf[i+Overlap] = sp[i] /32768.0;
		}
		waveInAddBuffer(hwi, pWaveHdr, sizeof(WAVEHDR));
		process_buffer(fbuf,SAMPLES_PER_BUFFER);
		memcpy(&fbuf[0],&fbuf[SAMPLES_PER_BUFFER],sizeof(float)*Overlap);
		break;
	case MM_WIM_OPEN:
		break;
	case MM_WIM_CLOSE:
		break;
	}
}

void input_sound(unsigned int sample_rate, unsigned int overlap, const char *ifname)
{
	Overlap = overlap;
	SAMPLES_PER_BUFFER = (BUFFER_LEN_IN_MS / 1000.0)*sample_rate;
	fbuf = (float*)malloc(sizeof(float)*(SAMPLES_PER_BUFFER+Overlap));
	hWavIn=0;
	g_WavFmt.wFormatTag = WAVE_FORMAT_PCM;
    g_WavFmt.nChannels = 1;
    g_WavFmt.nSamplesPerSec = sample_rate;
    g_WavFmt.wBitsPerSample = 16;
    g_WavFmt.nBlockAlign = g_WavFmt.nChannels * g_WavFmt.wBitsPerSample / 8;
    g_WavFmt.nAvgBytesPerSec = g_WavFmt.nChannels * g_WavFmt.wBitsPerSample / 8 * g_WavFmt.nSamplesPerSec;
    g_WavFmt.cbSize = 0;

    if (!waveInGetNumDevs())
    {
        printf("Default audio device not found!\n");
        exit(0);
    }
	
    DWORD ret = waveInOpen(&hWavIn, WAVE_MAPPER, &g_WavFmt, 0, 0, WAVE_FORMAT_QUERY);
    if (MMSYSERR_NOERROR != ret)
    {
        printf("Unsupported audio format.\n");
        exit(0);
    }
	ret = waveInOpen(&hWavIn, WAVE_MAPPER, &g_WavFmt,(DWORD_PTR) &waveInProc,0, CALLBACK_FUNCTION);
	WAVEHDR *headers[BUFFERS];
        int i;
        for (i=0; i< BUFFERS; i++)
	{
		WAVEHDR *WavHdr = (WAVEHDR*)malloc(sizeof(WAVEHDR));
		headers[i]=WavHdr;
		void* pBuffer = malloc(SAMPLES_PER_BUFFER*sizeof(SHORT));
		WavHdr->lpData = (LPSTR)pBuffer;
		WavHdr->dwBufferLength = SAMPLES_PER_BUFFER*sizeof(SHORT);
		WavHdr->dwBytesRecorded = 0;
		WavHdr->dwUser = 0;
		WavHdr->dwFlags = 0;
		WavHdr->dwLoops = 1;
		WavHdr->lpNext = 0;
		WavHdr->reserved = 0;
		MMRESULT x1= waveInPrepareHeader(hWavIn,WavHdr,sizeof(WAVEHDR));
		MMRESULT x2= waveInAddBuffer(hWavIn, WavHdr, sizeof(WAVEHDR));
	}
	MMRESULT x3=waveInStart(hWavIn);
	MMTIME MMTime = {0};   
	//Do nothing...till user gets tired of us and kills the app. 
	while(1)
	{
		Sleep(100);
	}
	waveInReset(hWavIn);
}
