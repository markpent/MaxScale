#include <sescmd.h>


SCMDLIST* sescmd_allocate()
{
    SCMDLIST* list;
    
    if((list = calloc(1,sizeof(SCMDLIST))) == NULL)
    {
        skygw_log_write(LOGFILE_ERROR,"Error : Memory allocation failed.");
        return NULL;
    }
    
    spinlock_init(&list->lock);
    list->semantics.reply_on = SRES_FIRST;
    list->semantics.must_reply = SNUM_ONE;
    list->semantics.on_error = SERR_DROP;
    
    return list;
}
/**
 * Free the session command list
 * @param list Session command list to free
 */
void sescmd_free(SCMDLIST*  list)
{
    SCMDCURSOR* cursor;
    SCMD* cmd;
    
    spinlock_acquire(&list->lock);
    cursor = list->cursors;
    cmd = list->first;
    list->cursors = NULL;
    list->first = NULL;
    list->last = NULL;
    spinlock_release(&list->lock);
    
    while(cmd)
    {
        SCMD* tmp = cmd;
        cmd = cmd->next;
        free(tmp);
    }
    
    while(cursor)
    {
        SCMDCURSOR* tmp = cursor;
        cursor = cursor->next;
        free(tmp);
    }
    
    free(list);
}

/**
 * Add a command to the list of session commands. This allocates a 
 * new SCMD structure that contains all the information the client side needs 
 * about this command.
 * @param scmdlist Session command list
 * @param buf Buffer with the session command to add
 * @return True if adding the command was successful. False on all errors.
 */
bool sescmd_add_command (SCMDLIST* scmdlist, GWBUF* buf)
{
   SCMDLIST* list = scmdlist;
   SCMD* cmd;
   
   if((cmd = calloc(1,sizeof(SCMD))) == NULL)
   {
       skygw_log_write(LOGFILE_ERROR,"Error : Memory allocation failed.");
       return false;
   }
   
   spinlock_init(&cmd->lock);
   cmd->buffer = gwbuf_clone(buf);
   cmd->packet_type = *((unsigned char*)buf->start + 4);
   cmd->reply_sent = false;
   
   if(list->first == NULL)
   {
       list->first = cmd;
       list->last = cmd;
   }
   else
   {
       list->last->next = cmd;
       list->last = cmd;
   }
   
   return true;
}

/**
 * Get the session command cursor associated with this DCB.
 * @param scmdlist Session command list
 * @param dcb DCB whose cursor we are looking for
 * @return Pointer to the cursor associated with this DCB or NULL if it was not found.
 */
SCMDCURSOR* get_cursor(SCMDLIST* scmdlist, DCB* dcb)
{
    SCMDCURSOR* cursor = scmdlist->cursors;
    
    while(cursor)
    {
        if(cursor->backend_dcb == dcb)
        {
            return cursor;
        }
        cursor = cursor->next;
    }
    return cursor;
}

/**
 * Get the GWBUF of the current command.
 * @param cursor Cursor to use
 * @return Pointer to the active command buffer or NULL if no session command is active
 */
GWBUF* sescmd_cursor_get_command(SCMDCURSOR* cursor)
{
    if(cursor->scmd_cur_cmd == NULL)
    {
        return NULL;
    }
    return cursor->scmd_cur_cmd->buffer;
}

/**
 * Check if the cursor is active.
 * @param cursor Cursor to check
 * @return True if the cursor is active. False if it is not.
 */
bool
sescmd_cursor_is_active(SCMDCURSOR* cursor)
{
    bool rval;
    spinlock_acquire(&cursor->lock);
    rval = cursor->scmd_cur_active;
    spinlock_release(&cursor->lock);
    return rval;
}

/**
 * Change the active state of the cursor
 * @param cursor Cursor to modify
 * @param value Activate or deactivate the cursor
 */
void
sescmd_cursor_set_active(SCMDCURSOR* cursor, bool value)
{
    spinlock_acquire(&cursor->lock);
    cursor->scmd_cur_active = value;
    spinlock_release(&cursor->lock);
}

/**
 * Check if the session command cursor associated with this backend DCB is already
 * executing session commands. If this is true, the backend server will automatically
 * repeat all the session commands after which it will go into inactive state.
 * @param list Session command list
 * @param dcb Backend server DCB
 * @return True if the backend server is already executing session commands and
 * false if it is inactive 
 */
bool sescmd_is_active(SCMDLIST* list, DCB* dcb)
{
    SCMDCURSOR* cursor = get_cursor(list,dcb);
    
    if(cursor == NULL)
	return false;
    
    return sescmd_cursor_is_active(cursor);
}

/**
 * See if the cursor has pending commands.
 * @param cursor Cursor to inspect
 * @return True if the cursor has pending commands. False if it has reached the end of the list.
 */
bool sescmd_has_next(SCMDLIST* list, DCB* dcb)
{
    SCMD* cmd;
    SCMDCURSOR* cursor;
    bool replied;
    
    cursor = get_cursor(list,dcb);
    
    if(cursor == NULL)
	return false;
    
    if(list->first == NULL)
    {
	/** No commands to execute */
	return false;
    }
    
    if(cursor->scmd_cur_cmd == NULL)
    {
        /** This is the first time this cursor is activated*/
        
        return true;
    }
    
    spinlock_acquire(&cursor->lock);
    
    cmd = cursor->scmd_cur_cmd ? cursor->scmd_cur_cmd->next : NULL;
    replied = cursor->replied_to;
    spinlock_release(&cursor->lock);
    if(cmd != NULL)
    {
        /** There are more commands to execute*/    
        
        return true;
    }
    
    if(replied == false)
    {
        /** The current command hasn't been replied to */
        return true;
    }
    
    /** This cursor has reached the end of the list*/
    
    return false;
}


/**
 * Move the cursor forward if it has not yet reached the end of the list.
 * @param list Session command list
 * @param dcb Backend DCB
 * @return Pointer to the next GWBUF containing the next command in the list or 
 * NULL if there are no commands to execute.
 */
GWBUF* sescmd_get_next(SCMDLIST* list, DCB* dcb)
{
    GWBUF* rval = NULL;
    SCMDCURSOR* cursor = get_cursor(list,dcb);
    
    if(cursor == NULL)
	return NULL;
    
    spinlock_acquire(&cursor->lock);
    
      if(cursor->scmd_list->first == NULL)
      {
	  /** No commands to execute */
	      rval = NULL;
	      goto retblock;
      }
    
    if(cursor->scmd_cur_cmd == NULL)
    {
        /** This is the first time this cursor is advanced */
        
        cursor->scmd_cur_cmd = cursor->scmd_list->first;
        cursor->replied_to = false;
        rval = sescmd_cursor_get_command(cursor);
        goto retblock;
    }
    
    if(cursor->scmd_cur_cmd->next && 
       cursor->replied_to)
    {
        /** There are pending commands and the current one received a response */    
        
        cursor->scmd_cur_cmd = cursor->scmd_cur_cmd->next;
        cursor->replied_to = false;
        rval = sescmd_cursor_get_command(cursor);
        goto retblock;
    }
    
    if(cursor->replied_to == false)
    {
        /** The current command is still active */

        rval = sescmd_cursor_get_command(cursor);
    }

    retblock:
    
    spinlock_release(&cursor->lock);
    
    return rval;
}

/**
 * Execute a pending session command in the backend server.
 * @param dcb Backend DCB where the command is executed
 * @param buffer GWBUF containing the session command
 * @return True if execution was successful or false if the write to the backend DCB failed.
 */
bool
sescmd_execute_in_backend(DCB* backend_dcb,GWBUF* buffer)
{
	DCB* dcb = backend_dcb;
	bool succp = true;
	int rc = 0;
	unsigned char packet_type;
	if(dcb == NULL)
	    return false;
	CHK_DCB(dcb);

	packet_type = MYSQL_GET_COMMAND(((unsigned char*)buffer->start));
	

#if defined(SS_DEBUG)
		{
			GWBUF* tmpbuf = gwbuf_clone(buffer);
			uint8_t* ptr = GWBUF_DATA(tmpbuf);
			unsigned char cmd = MYSQL_GET_COMMAND(ptr);

			skygw_log_write(
					LOGFILE_DEBUG,
					"%lu [execute_sescmd_in_backend] Just before write, fd "
					"%d : cmd %s.",
					pthread_self(),
					dcb->fd,
					STRPACKETTYPE(cmd));
			gwbuf_free(tmpbuf);
		}
#endif /*< SS_DEBUG */
		switch(packet_type)
		{
		case MYSQL_COM_CHANGE_USER:
			/** This makes it possible to handle replies correctly */
			gwbuf_set_type(buffer, GWBUF_TYPE_SESCMD);
			rc = dcb->func.auth(
					    dcb,
					    NULL,
					    dcb->session,
					    gwbuf_clone(buffer));
			break;

		case MYSQL_COM_INIT_DB:
		{
			/** Record database name and store to session. */
			GWBUF* tmpbuf;
			MYSQL_session* data;
			unsigned int qlen;

			data = dcb->session->data;
			tmpbuf = buffer;
			qlen = MYSQL_GET_PACKET_LEN((unsigned char*) tmpbuf->start);
			memset(data->db, 0, MYSQL_DATABASE_MAXLEN + 1);
			if(qlen > 0 && qlen < MYSQL_DATABASE_MAXLEN + 1)
				strncpy(data->db, tmpbuf->start + 5, qlen - 1);
		}
			/** Fallthrough */
		case MYSQL_COM_QUERY:
		default:
			/** 
			 * Mark session command buffer, it triggers writing 
			 * MySQL command to protocol
			 */
			gwbuf_set_type(buffer, GWBUF_TYPE_SESCMD);
			rc = dcb->func.write(
					     dcb,
					     gwbuf_clone(buffer));
			break;
		}

		if(rc == 1)
		{
			succp = true;
		}
		else
		{
			succp = false;
		}

	return succp;
}


/**
 * All cases where backend message starts at least with one response to session
 * command are handled here.
 * Read session commands from session command list. If command is already replied,
 * discard packet. Else send reply to client if the semantics of the list match. 
 * In both cases move cursor forward until all session command replies are handled. 
 * @param list Session command list
 * @param dcb Backend DCB
 * @param replybuf GWBUF containing the reply from the backend server
 * @return Pointer to a GWBUF that should be routed back to the client or NULL 
 * if no action needs to be taken.
 */
GWBUF* sescmd_process_replies(
        SCMDLIST* list,
        DCB* dcb,                            
        GWBUF*           replybuf)
{
    
        SCMD*  cmd;
        SCMDCURSOR* scur;
        bool return_reply = false;
        
        scur = get_cursor(list,dcb);
        cmd =  scur->scmd_cur_cmd;
        
        
        CHK_GWBUF(replybuf);
        
        /** 
         * Walk through packets in the message and the list of session 
         * commands. 
         */
        while (cmd != NULL && replybuf != NULL && return_reply == false)
        {
                /** Faster backend has already responded to client : discard */
                if (cmd->reply_sent)
                {
                        bool last_packet = false;
                        
                        CHK_GWBUF(replybuf);
                        
                        while (!last_packet)
                        {
                                int  buflen;
                                
                                buflen = GWBUF_LENGTH(replybuf);
                                last_packet = GWBUF_IS_TYPE_RESPONSE_END(replybuf);
                                /** discard packet */
                                replybuf = gwbuf_consume(replybuf, buflen);
                        }
                }
                /** Response is in the buffer and it will be sent to client. */
                else
                {
                    /** Mark the session command as replied */
                    
                    atomic_add(&cmd->n_replied,1);
                    
                    if(scur->scmd_list->semantics.reply_on == SRES_FIRST ||
                       (scur->scmd_list->semantics.reply_on == SRES_LAST && 
                        cmd->n_replied >= scur->scmd_list->n_cursors) || 
		       (scur->scmd_list->semantics.reply_on == SRES_MIN && 
                        cmd->n_replied >= scur->scmd_list->semantics.min_nreplies))
                    {
                        cmd->reply_sent = true;
                        return_reply = true;                        
                    }

                }

                /** Set response status received */
                scur->replied_to = true;                

                if (sescmd_has_next(list,dcb))
                {
		    /** This moves the cursor forwards */
		    sescmd_get_next(list,dcb);
		    cmd = scur->scmd_cur_cmd;
                }
                else
                {
                        cmd = NULL;
                        /** All session commands are replied */
                        sescmd_cursor_set_active(scur, false);
                }
        }
        
        return replybuf;
}

/**
 * Add a DCB to the session command list. This allocates a new session command 
 * cursor for this DCB and starts the execution of pending commands.
 * @param list Session command list
 * @param dcb DCB to add
 * @return True if adding the DCB was successful or the DCB was already in the list. 
 * False on all errors.
 */
bool sescmd_add_dcb (SCMDLIST* scmdlist, DCB* dcb)
{
    SCMDLIST* list = scmdlist;
    SCMDCURSOR* cursor;
    
    if(get_cursor(scmdlist,dcb) != NULL)
    {
	return true;
    }
    
    if((cursor = calloc(1,sizeof(SCMDCURSOR))) == NULL)
    {
        skygw_log_write(LOGFILE_ERROR,"Error : Memory allocation failed.");
        return false;
    }
    
    spinlock_init(&cursor->lock);
    cursor->backend_dcb = dcb;
    cursor->scmd_list = list;
    cursor->scmd_cur_cmd = list->first;
    cursor->next = list->cursors;
    list->cursors = cursor;
    atomic_add(&list->n_cursors,1);
    
    return true;
}

/**
 * Remove a DCB from the session command list.
 * @param list Session command list
 * @param dcb DCB to remove
 * @return True if removing the DCB was successful. False on all errors.
 */
bool sescmd_remove_dcb (SCMDLIST* scmdlist, DCB* dcb)
{
    SCMDLIST* list = scmdlist;
    SCMDCURSOR *cursor, *tmp;
    
    if((cursor = get_cursor(scmdlist,dcb)) == NULL)
    {
	return false;
    }
    
    spinlock_acquire(&cursor->lock);
    cursor->scmd_cur_active = false;
    cursor->scmd_cur_cmd = NULL;
    spinlock_release(&cursor->lock);
    
    spinlock_acquire(&list->lock);
    
    tmp = list->cursors;
    
    if(tmp == cursor)
    {
        list->cursors = cursor->next;
    }
    else
    {
        while(tmp && tmp->next != cursor)
            tmp = tmp->next;
        
        if(tmp)
        {
            tmp->next = cursor->next;
        }
        
    }
    spinlock_release(&list->lock);
    atomic_add(&list->n_cursors,-1);
    
    free(cursor);
    
    return true;
}

/**
 * To be implemented...
 * @param list
 * @param dcb
 * @return 
 */
bool sescmd_handle_failure(SCMDLIST* list, DCB* dcb)
{
    return true;
}