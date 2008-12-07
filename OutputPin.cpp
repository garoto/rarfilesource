/*
 * Copyright (C) 2008, OctaneSnail <os@v12pwr.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <streams.h>

#include "OutputPin.h"
#include "RFS.h"
#include "Utils.h"
#include "Anchor.h"


CRFSOutputPin::CRFSOutputPin (CRARFileSource *pFilter, CCritSec *pLock, HRESULT *phr) :
	CBasePin (L"RAR File Source Output Pin", pFilter, pLock, phr, L"Output", PINDIR_OUTPUT)
{
	m_align = 1;
	m_asked_for_reader = FALSE;
	m_file = NULL;
	m_flush = FALSE;

	if (!(m_event = CreateEvent (NULL, FALSE, FALSE, NULL)))
	{
		ErrorMsg (GetLastError (), L"CRFSOutputPin::CRFSOutputPin - CreateEvent");

		m_event = INVALID_HANDLE_VALUE;

		if (phr)
			*phr = S_FALSE;
	}
}

CRFSOutputPin::~CRFSOutputPin ()
{
	CloseHandle (m_event);
}

STDMETHODIMP CRFSOutputPin::NonDelegatingQueryInterface (REFIID riid, void **ppv)
{
	if (riid == IID_IAsyncReader)
	{
		m_asked_for_reader = TRUE;
		return GetInterface ((IAsyncReader*) this, ppv);
	}
	else
		return CBasePin::NonDelegatingQueryInterface (riid, ppv);
}

STDMETHODIMP CRFSOutputPin::Connect (IPin * pReceivePin, const AM_MEDIA_TYPE *pmt)
{
	return CBasePin::Connect(pReceivePin, pmt);
}

HRESULT CRFSOutputPin::GetMediaType (int iPosition, CMediaType *pMediaType)
{
	if (!pMediaType)
		return E_POINTER;

	if (!m_file)
		return E_UNEXPECTED;

	if (iPosition < 0)
		return E_INVALIDARG;

	if (iPosition > 0)
		return VFW_S_NO_MORE_ITEMS;

	*pMediaType = m_file->media_type;

	return S_OK;
}


HRESULT CRFSOutputPin::CheckMediaType (const CMediaType* pType)
{
	if (!m_file)
		return E_UNEXPECTED;

	// Treat MEDIASUBTYPE_NULL subtype as a wild card.
	if ((m_file->media_type.majortype == pType->majortype) &&
		(m_file->media_type.subtype == MEDIASUBTYPE_NULL || m_file->media_type.subtype == pType->subtype))
	{
		return S_OK;
	}

	return S_FALSE;
}

HRESULT CRFSOutputPin::CheckConnect (IPin *pPin)
{
	m_asked_for_reader = FALSE;
	return CBasePin::CheckConnect (pPin);
}

HRESULT CRFSOutputPin::CompleteConnect (IPin *pReceivePin)
{
	if (m_asked_for_reader)
		return CBasePin::CompleteConnect (pReceivePin);

	return VFW_E_NO_TRANSPORT;
}

HRESULT CRFSOutputPin::BreakConnect ()
{
	m_asked_for_reader = FALSE;
	return CBasePin::BreakConnect ();
}

STDMETHODIMP CRFSOutputPin::RequestAllocator (IMemAllocator *pPreferred, ALLOCATOR_PROPERTIES *pProps, IMemAllocator **ppActual)
{
	if (!(pPreferred && pProps && ppActual))
		return E_POINTER;

	ALLOCATOR_PROPERTIES actual;
	HRESULT hr;

	DbgLog((LOG_TRACE, 2, L"Requested alignment = %d", pProps->cbAlign));
	if (pProps->cbAlign)
		m_align = pProps->cbAlign;
	else
		pProps->cbAlign = m_align;

	if (pPreferred)
	{
		hr = pPreferred->SetProperties (pProps, &actual);

		if (SUCCEEDED (hr) && IsAligned (actual.cbAlign))
		{
			DbgLog((LOG_TRACE, 2, L"Using preferred allocator."));
			pPreferred->AddRef ();
			*ppActual = pPreferred;
			return S_OK;
		}
	}

	CMemAllocator *pMemObject = new CMemAllocator(L"RFS memory allocator", NULL, &hr);

	if (!pMemObject)
		return E_OUTOFMEMORY;

	if (FAILED (hr))
	{
		delete pMemObject;
		return hr;
	}

	IMemAllocator* pAlloc;

	hr = pMemObject->QueryInterface (IID_IMemAllocator, (void **) &pAlloc);

	if (FAILED (hr))
	{
		delete pMemObject;
		return E_NOINTERFACE;
	}

	hr = pAlloc->SetProperties (pProps, &actual);

	if (SUCCEEDED (hr) && IsAligned (actual.cbAlign))
	{
		DbgLog((LOG_TRACE, 2, L"Using our allocator."));
		*ppActual = pAlloc;
		return S_OK;
	}

	pAlloc->Release ();

	if (SUCCEEDED (hr))
		hr = VFW_E_BADALIGN;

	DbgLog ((LOG_TRACE, 2, L"RequestAllocator failed."));
	return hr;
}

HRESULT CRFSOutputPin::ConvertSample (IMediaSample* sample, LONGLONG *pos, DWORD *length, BYTE **buffer)
{
	if (!(sample && pos && length && buffer))
		return E_POINTER;

	REFERENCE_TIME start, stop;

	HRESULT hr = sample->GetTime (&start, &stop);
	if (FAILED (hr))
		return hr;

	if (start < 0)
		return E_UNEXPECTED;

	LONGLONG llPos = start / UNITS;
	LONGLONG llLength = (stop - start) / UNITS;

	if (llLength < 0 || llLength > LONG_MAX)
		return E_UNEXPECTED;

	DWORD lLength = (LONG) llLength;
	LONGLONG llTotal = m_file->size;

	if (llPos > llTotal)
	{
		DbgLog((LOG_TRACE, 2, L"ConvertSample EOF pos = %d total = %d", llPos, llTotal));
		return ERROR_HANDLE_EOF;
	}

	if (llPos + lLength > llTotal)
	{
		llTotal = (llTotal + m_align - 1) & ~((LONGLONG) (m_align - 1));

		if (llPos + lLength > llTotal)
		{
			lLength = (LONG) (llTotal - llPos);

			stop = llTotal * UNITS;
			sample->SetTime (&start, &stop);
		}
	}

	BYTE* b;
	hr = sample->GetPointer (&b);
	if (FAILED (hr))
	{
		DbgLog((LOG_TRACE, 2, L"ConvertSample pSample->GetPointer failed"));
		return hr;
	}

	*pos = llPos;
	*length = lLength;
	*buffer = b;

	return S_OK;
}

STDMETHODIMP CRFSOutputPin::Request (IMediaSample* pSample, DWORD_PTR dwUser)
{
	LONGLONG llPosition;
	DWORD lLength;
	BYTE* pBuffer;

	if (m_flush)
	{
		DbgLog((LOG_TRACE, 2, L"Request called during flush."));
		return VFW_E_WRONG_STATE;
	}

	if (!m_file)
	{
		DbgLog((LOG_TRACE, 2, L"Request called with no file loaded."));
		return E_UNEXPECTED;
	}

	HRESULT hr = ConvertSample (pSample, &llPosition, &lLength, &pBuffer);

	if (FAILED (hr))
		return hr;

	if(!(IsAligned (llPosition) && IsAligned (lLength) && IsAligned ((INT_PTR) pBuffer)))
	{
		DbgLog((LOG_TRACE, 2, L"SyncReadAligned bad alignment. align = %d, pos = %d, len = %d, buf = %p",
			m_align, llPosition, lLength, pBuffer));
        return VFW_E_BADALIGN;
	}

	LARGE_INTEGER offset;
	DWORD to_read, acc = 0, offset2;
	int pos = FindStartPart (llPosition);

	if (pos == -1)
		return S_FALSE;

	ReadRequest *request = new ReadRequest ();

	if (!request)
		return E_OUTOFMEMORY;

	Anchor<ReadRequest> arr (&request);

	request->dwUser = dwUser;
	request->pSample = pSample;
	request->count = 0;

	FilePart *part = m_file->array + pos;

	offset2 = (DWORD) (llPosition - part->in_file_offset);
	offset.QuadPart = part->in_rar_offset + offset2;

	while (true)
	{
		SubRequest *sr = new SubRequest ();

		if (!sr)
		{
			ErrorMsg (0, L"Out of memory.");
			return E_OUTOFMEMORY;
		}

		request->subreqs.InsertLast (sr);
		request->count ++;

		to_read = min (lLength, part->size - offset2);

		sr->file = part->file;
		sr->expected = to_read;
		sr->o.hEvent = CreateEvent (NULL, FALSE, FALSE, NULL);

		if (!sr->o.hEvent)
		{
			sr->o.hEvent = INVALID_HANDLE_VALUE;
			return S_FALSE;
		}

		sr->o.Offset = offset.LowPart;
		sr->o.OffsetHigh = offset.HighPart;

		if (!ReadFile (part->file, pBuffer + acc, to_read, NULL, &sr->o))
		{
			DWORD err = GetLastError ();

			// FIXME: Do something smart in response to EOF.
			if (err != ERROR_IO_PENDING && err != ERROR_HANDLE_EOF)
			{
				ErrorMsg (err, L"CRFSOutputPin::Request - ReadFile");
				return S_FALSE;
			}
		}
		lLength -= to_read;
		acc += to_read;

		if (lLength <= 0)
			break;

		pos ++;

		if (pos >= m_file->parts)
			return S_FALSE;

		part ++;
		offset2 = 0;
		offset.QuadPart = part->in_rar_offset;
	}

	m_lock.Lock ();

	arr.Release ();
	m_requests.InsertFirst (request);

	if (!SetEvent (m_event))
		ErrorMsg (GetLastError (), L"CRFSOutputPin::Request - SetEvent");

	m_lock.Unlock ();

	return S_OK;
}

HRESULT CRFSOutputPin::DoFlush (IMediaSample **ppSample, DWORD_PTR *pdwUser)
{
	ReadRequest *rr;
	SubRequest *sr;

	DbgLog ((LOG_TRACE, 2, L"WaitForNext is flushing..."));

	m_lock.Lock ();
	rr = m_requests.UnlinkLast ();
	m_lock.Unlock ();

	if (!rr)
	{
		*ppSample = NULL;
		return VFW_E_TIMEOUT;
	}

	while (sr = rr->subreqs.UnlinkLast ())
	{
		CancelIo (sr->file);
		delete sr;
	}

	*pdwUser = rr->dwUser;
	*ppSample = rr->pSample;

	delete rr;

	return VFW_E_TIMEOUT;
}

STDMETHODIMP CRFSOutputPin::WaitForNext (DWORD dwTimeout, IMediaSample **ppSample, DWORD_PTR *pdwUser)
{
	HRESULT ret = S_OK;
	DWORD r;
	ReadRequest *rr;

	if (!(ppSample && pdwUser))
		return E_POINTER;

	if (m_flush)
		return DoFlush (ppSample, pdwUser);

	m_lock.Lock ();
	rr = m_requests.UnlinkLast ();
	m_lock.Unlock ();

	Anchor<ReadRequest> arr (&rr);

	while (!rr)
	{
		r = WaitForSingleObject (m_event, dwTimeout);

		if (m_flush)
			return DoFlush (ppSample, pdwUser);

		if (r == WAIT_TIMEOUT)
			return VFW_E_TIMEOUT;

		if (r == WAIT_FAILED)
		{
			ErrorMsg (GetLastError (), L"CRFSOutputPin::WaitForNext - WaitForSingleObject");
			return E_FAIL;
		}

		m_lock.Lock ();
		rr = m_requests.UnlinkLast ();
		m_lock.Unlock ();

		if (!rr)
			DbgLog ((LOG_TRACE, 2, L"Got nothing?!?!"));
	}

	DWORD count, read, acc = 0;
	SubRequest *sr = rr->subreqs.First ();

	count = rr->count;

	HANDLE *hArray = new HANDLE [count];

	for (DWORD i = 0; i < count; i ++)
	{
		hArray [i] = sr->o.hEvent;
		sr = rr->subreqs.Next (sr);
	}

	// FIXME: Any time spent waiting in WaitForSingleObject above should be subtracted from dwTimeout
	r = WaitForMultipleObjects (count, hArray, TRUE, dwTimeout);

	delete hArray;

	if (r == WAIT_TIMEOUT)
	{
		// Put it back into the list.
		m_lock.Lock ();
		arr.Release ();
		m_requests.InsertLast (rr);
		m_lock.Unlock ();
		return VFW_E_TIMEOUT;
	}

	*pdwUser = rr->dwUser;
	*ppSample = rr->pSample;

	if (r == WAIT_FAILED)
	{
		ErrorMsg (GetLastError (), L"CRFSOutputPin::WaitForNext - WaitForMultipleObjects");
		return E_FAIL;
	}

	while (sr = rr->subreqs.UnlinkFirst ())
	{
		read = 0;

		if (!GetOverlappedResult (sr->file, &sr->o, &read, TRUE))
		{
			ErrorMsg (GetLastError (), L"CRFSOutputPin::WaitForNext - GetOverlappedResult");
			acc += read;
			delete sr;
			ret = S_FALSE; // FIXME: Should probably return EOF if that's what happened.
			break;
		}

		acc += read;

		// TODO: Try to recover if read != sr->expected.
		if (read != sr->expected)
		{
			DbgLog((LOG_TRACE, 2, L"CRFSOutputPin::WaitForNext Got %d expected %d!", read, sr->expected));
			delete sr;
			ret = S_FALSE;
			break;
		}
		delete sr;
	}

	rr->pSample->SetActualDataLength (acc);

	return ret;
}


STDMETHODIMP CRFSOutputPin::SyncReadAligned (IMediaSample* pSample)
{
	LONGLONG llPosition;
	DWORD lLength;
	BYTE* pBuffer;

	if (!m_file)
	{
		DbgLog((LOG_TRACE, 2, L"SyncRead called with no file loaded."));
		return E_UNEXPECTED;
	}

	HRESULT hr = ConvertSample (pSample, &llPosition, &lLength, &pBuffer);

	if (FAILED (hr))
		return hr;

	if(!(IsAligned (llPosition) && IsAligned (lLength) && IsAligned ((INT_PTR) pBuffer)))
	{
		DbgLog((LOG_TRACE, 2, L"SyncReadAligned bad alignment. align = %d, pos = %d, len = %d, buf = %p",
			m_align, llPosition, lLength, pBuffer));
        return VFW_E_BADALIGN;
	}

	LONG cbActual = 0;

	hr = SyncRead (llPosition, lLength, pBuffer, &cbActual);

	pSample->SetActualDataLength (cbActual);

	return hr;
}

STDMETHODIMP CRFSOutputPin::SyncRead (LONGLONG llPosition, LONG lLength, BYTE* pBuffer)
{
	if (lLength < 0)
		return E_UNEXPECTED;

	return SyncRead (llPosition, lLength, pBuffer, NULL);
}

HRESULT CRFSOutputPin::SyncRead (LONGLONG llPosition, DWORD lLength, BYTE* pBuffer, LONG *cbActual)
{
	OVERLAPPED o;
	LARGE_INTEGER offset;
	DWORD to_read, read, acc = 0, offset2;
	int pos;
#ifdef _DEBUG
	static int last_pos = -1;
#endif

	if (!m_file)
	{
		DbgLog((LOG_TRACE, 2, L"SyncRead called with no file loaded."));
		return E_UNEXPECTED;
	}

	if (!pBuffer)
		return E_POINTER;

	pos = FindStartPart (llPosition);
	if (pos == -1)
	{
		DbgLog((LOG_TRACE, 2, L"FindStartPart bailed length = %d, pos = %d", lLength, llPosition));
		return ERROR_HANDLE_EOF;
	}

#ifdef _DEBUG
	if (pos != last_pos)
	{
		DbgLog((LOG_TRACE, 2, L"Now reading file %d.", pos));
		last_pos = pos;
	}
#endif
	FilePart *part = m_file->array + pos;

	offset2 = (DWORD) (llPosition - part->in_file_offset);
	offset.QuadPart = part->in_rar_offset + offset2;

	memset (&o, 0, sizeof (o));

	if (!(o.hEvent = CreateEvent (NULL, FALSE, FALSE, NULL)))
	{
		ErrorMsg (GetLastError (), L"CRFSOutputPin::SyncRead - CreateEvent)");
		return (S_FALSE);
	}

	while (true)
	{
		read = 0;
		to_read = min (lLength, part->size - offset2);

		o.Offset = offset.LowPart;
		o.OffsetHigh = offset.HighPart;

		if (!ReadFile (part->file, pBuffer + acc, to_read, NULL, &o))
		{
			DWORD err = GetLastError ();

			if (err != ERROR_IO_PENDING)
			{
				ErrorMsg (err, L"CRFSOutputPin::SyncRead - ReadFile");
				break;
			}
		}
		if (!GetOverlappedResult (part->file, &o, &read, TRUE))
		{
			ErrorMsg (GetLastError (), L"CRFSOutputPin::SyncRead - GetOverlappedResult)");
			break;
		}
		lLength -= read;
		acc += read;

		if (lLength == 0)
		{
			CloseHandle (o.hEvent);
			if (cbActual)
				*cbActual = acc;
			return S_OK;
		}

		pos ++;

		if (pos >= m_file->parts)
			break;

		part ++;
		offset2 = 0;
		offset.QuadPart = part->in_rar_offset;
	}

	CloseHandle (o.hEvent);
	if (cbActual)
		*cbActual = acc;
	return S_FALSE;
}

STDMETHODIMP CRFSOutputPin::Length (LONGLONG *pTotal, LONGLONG *pAvailable)
{
	if (!m_file)
		return E_UNEXPECTED;

	if (pTotal)
		*pTotal = m_file->size;

	if (pAvailable)
		*pAvailable = m_file->size;

	return S_OK;
}

STDMETHODIMP CRFSOutputPin::BeginFlush (void)
{
	DbgLog ((LOG_TRACE, 2, L"CRFSOutputPin::BeginFlush"));
	m_flush = TRUE;
	SetEvent (m_event);
	return S_OK;
}


STDMETHODIMP CRFSOutputPin::EndFlush (void)
{
	DbgLog ((LOG_TRACE, 2, L"CRFSOutputPin::EndFlush"));
	m_flush = FALSE;
	return S_OK;
}

static int compare (const void *pos, const void *part)
{
	if (*((LONGLONG *) pos) < ((FilePart *) part)->in_file_offset)
		return -1;

	if (*((LONGLONG *) pos) >= ((FilePart *) part)->in_file_offset + ((FilePart *) part)->size)
		return 1;

	return 0;
}

int CRFSOutputPin::FindStartPart (LONGLONG position)
{
	if (position > m_file->size)
		return -1;

	// Check if the previous lookup up still matches.
	if (m_prev_part && !compare (&position, m_prev_part))
		return (int) (m_prev_part - m_file->array);

	m_prev_part = (FilePart *) bsearch (&position, m_file->array, m_file->parts, sizeof (FilePart), compare);

	if (!m_prev_part)
		return -1;

	return (int) (m_prev_part - m_file->array);
}