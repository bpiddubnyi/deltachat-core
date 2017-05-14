/*******************************************************************************
 *
 *                             Messenger Backend
 *                      Copyright (C) 2017 Björn Petersen
 *                   Contact: r10s@b44t.com, http://b44t.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see http://www.gnu.org/licenses/ .
 *
 *******************************************************************************
 *
 * File:    mre2ee_driver_bsd.c
 * Purpose: End-To-End-Encryption based upon BSD's netpgp.
 *
 *******************************************************************************
 *
 * If we want to switch to other encryption engines, here are the functions to
 * be replaced.
 *
 * However, eg. GpgME cannot (easily) be used standalone and GnuPG's licence
 * would not allow the original creator of Delta Chat to release a proprietary
 * version, which, however, is required for the Apple store. (NB: the original
 * creator is the only person who could do this, a normal licensee is not
 * allowed to do so at all)
 *
 * So, we do not see a simple alternative - but everyone is welcome to implement
 * one :-)
 *
 ******************************************************************************/


#include <string.h>
#include "mrmailbox.h"
#include "mre2ee.h"
#include "mre2ee_driver.h"


void mre2ee_driver_init(mrmailbox_t* mailbox)
{
}


void mre2ee_driver_exit(mrmailbox_t* mailbox)
{
}


void mre2ee_driver_encrypt__(mrmailbox_t* mailbox, const clist* recipients_addr, struct mailmime** in_out_message)
{
	if( mailbox == NULL || recipients_addr == NULL || in_out_message == NULL || *in_out_message == NULL ) {
		return;
	}
}


void mre2ee_driver_decrypt__(mrmailbox_t* mailbox, struct mailmime** in_out_message)
{
	if( mailbox == NULL || in_out_message == NULL || *in_out_message == NULL ) {
		return;
	}
}