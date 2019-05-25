/*
 *   IRC - Internet Relay Chat, src/modules/labeled-response.c
 *   (C) 2019 Syzop & The UnrealIRCd Team
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "unrealircd.h"

ModuleHeader MOD_HEADER(labeled-response)
  = {
	"labeled-response",
	"4.2",
	"Labeled response CAP",
	"3.2-b8-1",
	NULL 
	};

/* Forward declarations */
int lr_pre_command(aClient *from, MessageTag *mtags, char *buf);
int lr_post_command(aClient *from, MessageTag *mtags, char *buf);
int lr_packet(aClient *from, aClient *to, aClient *intended_to, char **msg, int *len);

#define BATCHLEN 8

/* Our special version assumes that remote servers always handle it */
#define SupportBatch(x)		(MyConnect(x) ? HasCapability((x), "batch") : 1)

struct {
	aClient *client; /**< The client who issued the original command with a label */
	char label[256]; /**< The label attached to this command */
	char batch[BATCHLEN+1]; /**< The generated batch id */
	int responses; /**< Number of lines sent back to client */
	int sent_remote; /**< Command has been sent to remote server */
	int version; /**< Which version? Zero for official, non-zero is draft */
} currentcmd;

long CAP_LABELED_RESPONSE = 0L;

int labeled_response_mtag_is_ok(aClient *acptr, char *name, char *value);

MOD_INIT(labeled-response)
{
	ClientCapabilityInfo cap;
	ClientCapability *c;
	MessageTagHandlerInfo mtag;

	MARK_AS_OFFICIAL_MODULE(modinfo);

	memset(&currentcmd, 0, sizeof(currentcmd));

	memset(&cap, 0, sizeof(cap));
	cap.name = "draft/labeled-response";
	c = ClientCapabilityAdd(modinfo->handle, &cap, &CAP_LABELED_RESPONSE);

	memset(&mtag, 0, sizeof(mtag));
	mtag.name = "label";
	mtag.is_ok = labeled_response_mtag_is_ok;
	mtag.clicap_handler = c;
	MessageTagHandlerAdd(modinfo->handle, &mtag);

	HookAdd(modinfo->handle, HOOKTYPE_PRE_COMMAND, 2000000000, lr_pre_command);
	HookAdd(modinfo->handle, HOOKTYPE_POST_COMMAND, -2000000000, lr_post_command);
	HookAdd(modinfo->handle, HOOKTYPE_PACKET, 0, lr_packet);
	return MOD_SUCCESS;
}

MOD_LOAD(labeled-response)
{
	return MOD_SUCCESS;
}

MOD_UNLOAD(labeled-response)
{
	return MOD_SUCCESS;
}

int lr_pre_command(aClient *from, MessageTag *mtags, char *buf)
{
	memset(&currentcmd, 0, sizeof(currentcmd));

	for (; mtags; mtags = mtags->next)
	{
		if ((!strcmp(mtags->name, "draft/label") || !strcmp(mtags->name, "label")) && mtags->value)
		{
			if (!strcmp(mtags->name, "draft/label"))
				currentcmd.version = -1;
			strlcpy(currentcmd.label, mtags->value, sizeof(currentcmd.label));
			currentcmd.client = from;
			break;
		}
	}

	return 0;
}

char *labeled_response_batch_type(int version)
{
	if (version == 0)
		return "labeled-response";
	else
		return "draft/labeled-response";
}

char *labeled_response_message_tag(int version)
{
	if (version == 0)
		return "label";
	else
		return "draft/label";
}

void generate_batch_id(char *str)
{
	int i, v;
	for (i = 0; i < BATCHLEN; i++)
	{
		v = getrandom8() % (26+26+10);
		if (v < 26)
			*str++ = 'a'+v;
		else if (v < 52)
			*str++ = 'A'+(v-26);
		else
			*str++ = '0'+(v-52);
	}
	*str = '\0';
}

char *gen_start_batch(void)
{
	static char buf[512];

	generate_batch_id(currentcmd.batch);

	if (MyConnect(currentcmd.client))
	{
		/* Local connection */
		snprintf(buf, sizeof(buf), "@%s=%s :%s BATCH +%s %s",
			labeled_response_message_tag(currentcmd.version),
			currentcmd.label,
			me.name,
			currentcmd.batch,
			labeled_response_batch_type(currentcmd.version));
	} else {
		/* Remote connection: requires intra-server BATCH syntax */
		snprintf(buf, sizeof(buf), "@%s=%s :%s BATCH %s +%s %s",
			labeled_response_message_tag(currentcmd.version),
			currentcmd.label,
			me.name,
			currentcmd.client->name,
			currentcmd.batch,
			labeled_response_batch_type(currentcmd.version));
	}
	return buf;
}

int lr_post_command(aClient *from, MessageTag *mtags, char *buf)
{
	/* We may have to start or end a BATCH here, if all of
	 * the following is true:
	 * 1. The client is still online (from is not NULL)
	 * 2. A "label" was attached
	 * 3. The client supports BATCH (or is remote)
	 * 4. The command has not been forwarded to a remote server
	 *    (in which case they would be handling it, and not us)
	 */
	if (from && currentcmd.client && SupportBatch(from) &&
	    !(currentcmd.sent_remote && !currentcmd.responses))
	{
		aClient *savedptr;

		if (currentcmd.responses == 0)
		{
			/* Start the batch now (only to be terminated right after) */
			// XXX: if this changes in the spec, then send something
			//      different for the 0-replies-case.
			sendto_one(from, NULL, "%s", gen_start_batch());
		}

		/* End the batch */
		savedptr = currentcmd.client;
		currentcmd.client = NULL;
		if (MyConnect(savedptr))
			sendto_one(from, NULL, ":%s BATCH -%s", me.name, currentcmd.batch);
		else
			sendto_one(from, NULL, ":%s BATCH %s -%s", me.name, savedptr->name, currentcmd.batch);
	}
	memset(&currentcmd, 0, sizeof(currentcmd));
	return 0;
}

int lr_packet(aClient *from, aClient *to, aClient *intended_to, char **msg, int *len)
{
	static char packet[8192];
	char buf[512];

	if (currentcmd.client)
	{
		/* Labeled response is active */
		if (currentcmd.client == intended_to)
		{
			/* Add the label */
			// XXX: TODO: this currently assumes 0 tags already exist, BAD !!!!
			if (currentcmd.responses == 0)
			{
				/* Start the batch now, normally this would be a sendto_one()
				 * but doing so is not possible since we are in the sending code ;)
				 */
				char *batchstr = gen_start_batch();
				snprintf(packet, sizeof(packet),
				         "%s\r\n"
				         "@batch=%s %s",
				         batchstr,
				         currentcmd.batch,
				         *msg);
				*msg = packet;
				*len = strlen(*msg);
			} else {
				snprintf(packet, sizeof(packet), "@batch=%s ", currentcmd.batch);
				strlcat(packet, *msg, sizeof(packet));
				*msg = packet;
				*len = strlen(*msg);
			}
			currentcmd.responses++;
		}
		else if (IsServer(to) || !MyClient(to))
		{
			currentcmd.sent_remote = 1;
		}
	}

	return 0;
}

/** This function verifies if the client sending the
 * tag is permitted to do so and uses a permitted syntax.
 */
int labeled_response_mtag_is_ok(aClient *acptr, char *name, char *value)
{
	if (IsServer(acptr))
		return 1;

	/* Do some basic sanity checking for non-servers */
	if (value && strlen(value) <= 64)
		return 1;

	return 0;
}
