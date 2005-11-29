/** \file ac_gdb.cpp
 * GDB Support for ArchC simulators.
 *
 *    This file implements AC_GDB methods to support GDB communication protocol
 * (see info gdb / "Remote Protocol"). The current implementation supports the
 * following gdb commands:
 *
 \verbatim
 .----------------.---------------------------------------.-----------------.
 | Command        | Function Description                  | Return value    |
 }----------------+---------------------------------------+-----------------{
 | g              | Return the value of the CPU registers | hex data or ENN |
 | G              | Set the value of the CPU registers    | OK or ENN       |
 |                |                                       |                 |
 | mAA..AA,LLLL   | Read LLLL bytes at address AA..AA     | hex data or ENN |
 | MAA..AA,LLLL:  | Write LLLL bytes at address AA.AA     | OK or ENN       |
 |                |                                       |                 |
 | c              | Resume at current address             | SNN (signal NN) |
 | cAA..AA        | Continue at address AA..AA            | SNN             |
 |                |                                       |                 |
 | s              | Step one instruction                  | SNN             |
 | sAA..AA        | Step one instruction from AA..AA      | SNN             |
 |                |                                       |                 |
 | k              | kill                                  |                 |
 |                |                                       |                 |
 | ZT,AA..AA,LLLL | Insert breakpoint or watchpoint       | OK, ENN or ''   |
 | zT,AA..AA,LLLL | Remove breakpoint or watchpoint       | OK, ENN or ''   |
 |                |                                       |                 |
 | ?              | What was the last sigval ?            | SNN             |
 |                |                                       |                 |
 | 0x03           | Control-C                             |                 |
 `----------------'---------------------------------------'-----------------'
 \endverbatim
 *
 *    All commands and responses are sent with a packet which includes a
 * checksum.	A packet consists of
 *
 \verbatim
                 $<packet info>#<checksum>.
 \endverbatim
 *
 * where
 *   packet info = characters representing the command or response
 *   checksum    = two hex digits computed as modulo 256 sum of packet info
 *
 *    When a packet is received, it is first acknowledged with either '+' or '-'
 * '+' indicates a successful transfer.	 '-' indicates a failed transfer.
 *
 *    This file is to be processor agnostic!  Every code that depends on 
 * processor specific features must be handled in AC_GDB_Interface.
 *
 *******************************************************************************
 *
 * \note When modifing this file respect:
 * \li License
 * \li Previous author names. Add your own after current ones.
 * \li Coding style (basically emacs style)
 * \li Commenting style. This code use doxygen (http://www.doxygen.org)
 *     to be documented.
 *
 * \note This code contains code from others, see the helper functions below for
 *       origin references and copyright notices. Those code lines come from 
 *       Linux kernel and are under GNU-GPL.
 *
 *
 * \todo Right now, just memory breakpoints are supported, the other (hardware,
 *       write, read and access) are not implemented. They are marked as:
 *           \code // FIXME --- not yet supported \endcode
 *       If you want to improve GDB support, try to implement these.
 *
 *
 *******************************************************************************
 *                                                                             
 * LICENSE:
 *    GNU GPL --- General Public License, version 2.0 or greater.
 *    See archc/COPYING for more information.
 *
 * \author Daniel Cabrini Hauagge    <ra008388@ic.unicamp.br>
 * \author Gustavo Sverzut Barbieri  <ra008849@ic.unicamp.br>
 * \author Joao Victor Andrade Neves <ra008951@ic.unicamp.br>
 * \author Rafael Dantas de Castro   <ra009663@ic.unicamp.br>
 *
 * NOTICE:
 *    Contains extracts from Linux Kernel code. See below.
 *
 *******************************************************************************
 */

#include "ac_gdb.H"


/**
 * Constructor: create a GDB communication instance with the 
 * processor specific functions, implemented in \a proc. The simulator
 * will then listen for GDB connections at \a port
 *
 * \param proc processor specific functions that know how to read/write memory/registers.
 * \param port which port to listen for connections
 *
 * \return new AC_GDB instance
 */
AC_GDB::AC_GDB(AC_GDB_Interface* proc, int port) {
  this->connected  = 0;
  this->step       = 0;
  this->first_time = 1;
  this->proc       = proc;
  this->bps= new Breakpoints( BREAKPOINTS );
  this->set_port( port );
  this->disable();
}


/**
 * Destructor: Destroy AC_GDB instance
 */
AC_GDB::~AC_GDB() {
  delete bps;
  debug( "AC_GDB: connection closed!" );
}



/* Register Access ***********************************************************/

/**
 * Read registers to GDB.
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::reg_read(char *ib, char *ob) {
  int i;
  ob[ 0 ] = '\0';
  char fmt[ 5 ] = "";
  ac_word r;

  /* 1 byte is represented in hexadecimal by 2 chars */
  snprintf( fmt, 5, "%%%02dx", sizeof( ac_word ) * 2 );

  for ( i = 0; i < proc->nRegs(); i ++ )
    {
      r = proc->reg_read( i );
      if ( ac_resources::ac_tgt_endian == 0 )
	r = changeendianess( r );
      sprintf( ob + strlen( ob ), fmt, r );
    }
}


 /**
 * Write registers with GDB input.
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::reg_write(char *ib, char *ob) {
  ac_word aux;
  int i, ss;
  char fmt[ 5 ] = "";

  ib ++; /* skip G */

  /* 1 byte is represented in hexadecimal by 2 chars */
  ss = sizeof( ac_word ) * 2;
  snprintf( fmt, 5, "%%%02dx", ss );

  for ( i = 0; i < 32; i ++) {
    sscanf( ib, fmt, &aux );
    if ( ac_resources::ac_tgt_endian == 0 )
      aux = changeendianess( aux );
    proc->reg_write( i, aux );
    ib += ss;
  }

  strncpy( ob, "OK", GDB_BUFFERSIZE );
}



/* Memory Access *************************************************************/

/**
 * Write simulator memory with data provided by GDB.
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::mem_write( char *ib, char *ob ) {
  unsigned i, r;
  unsigned address, bytes;
  char *ib_h = ib;
  char fmt[ 5 ] = "%02x";
  ac_Uword n;

  r = sscanf( ib, "M%x,%x:", &address, &bytes );

  ib = strchr( ib, ':' );

  if ( ( ! ib ) || ( r != 2 ) )
    /* Data is wrong! */
    strncpy( ob, "E01", GDB_BUFFERSIZE );
  else {
    int offset;
    ib ++; /* next char after ':' */

    /* be sure to not read outside in_buffer */
    offset = ib - ib_h;
    if ( ( ( bytes << 1 ) + 1 + offset ) >= GDB_BUFFERSIZE )
      /* [ offset | hex bytes repr...	 | \0 ] <= GDB_BUFFERSIZE */
      bytes = ( ( GDB_BUFFERSIZE - offset ) >> 1 ) - 1;


    for ( i = 0; i < (bytes * 2); i += 2 )
      {
	if ( ib[ i ] == 0 )
	  strncpy( ob, "E03", GDB_BUFFERSIZE); /* end of string ('\0'), this is an error! */
	n=0;
	sscanf( ib, fmt, &n );
	ib += 2;
	
	proc->mem_write(address,n);
	address++;

      }
    strncpy( ob, "OK", GDB_BUFFERSIZE );
  }
}


/**
 * Read simulator memory.
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::mem_read( char *ib, char *ob ) {
  unsigned i;
  unsigned address = 0, bytes = 0;
  char fmt[ 5 ] = "%02x";
  unsigned int n = 0;

  if ( sscanf( ib, "m%x,%x", &address, &bytes ) != 2 )
    /* Data is wrong! */
    strncpy( ob, "E01", GDB_BUFFERSIZE );
  else {
    /* be sure there is enough room for returning string + '\0' */
    if ( ( ( bytes << 1 ) + 1 ) >= GDB_BUFFERSIZE )
      /* Read just bytes that fit the buffer */
      bytes = ( GDB_BUFFERSIZE >> 1 ) - 1;

    for ( i = 0; i < bytes; i++ )
      {
	n = 0;
	n = proc->mem_read( address );
	address ++;

	snprintf( ob + i * 2, GDB_BUFFERSIZE, fmt, n );
      }

    ob[ i * 2 ] = '\0';

  }
}





/* Execution Control *********************************************************/

/**
 * Have simulator to continue executing
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::continue_execution( char *ib, char *ob ) {
  unsigned address;

  if ( sscanf( ib, "c%x", &address ) == 1 )
    ac_resources::ac_pc = address;
  step = 0;
}


/**
 * Have simulator to go step by step
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::stepmode( char *ib, char *ob ) {
  unsigned address;
  if ( sscanf( ib, "s%x", &address ) == 1 )
    ac_resources::ac_pc = address;

  step = 1;
}


/**
 * Control-C: SIGINT and return control to gdb
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::cc( char *ib, char *ob ) {
  snprintf( ob, GDB_BUFFERSIZE, "S%02x", SIGINT );
  step = 1;
}


/**
 * Send exit status to GDB.
 *
 * \param ac_exit_status exit status from process.
 */
void AC_GDB::exit(int ac_exit_status) {
  if ( disabled ) return;
  snprintf( out_buffer, GDB_BUFFERSIZE, "W%02x", ac_exit_status );
  comm_putpacket(out_buffer);
}


/* Break & Watch Point *******************************************************/

/**
 * Insert Break or Watch point.
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::break_insert( char *ib, char *ob ) {
  int type, address, length;

  if ( sscanf( ib, "Z%x,%x,%x", &type, &address, &length ) != 3 )
    /* Data is wrong! */
    strncpy( ob, "E01", GDB_BUFFERSIZE );
  else {
    switch ( type ) {
    case 0:
      /* memory breakpoint */
      if ( bps->add( address ) == 0 )
	strncpy( ob, "OK", GDB_BUFFERSIZE );
      else
	strncpy( ob, "E00", GDB_BUFFERSIZE );
      break;

    case 1:
      /* hardware breakpoint */
      ob[ 0 ] = 0; /* FIXME --- not yet supported */
      break;

    case 2:
      /* write watchpoint */
      ob[ 0 ] = 0; /* FIXME --- not yet supported */
      break;

    case 3:
      /* read watchpoint */
      ob[ 0 ] = 0; /* FIXME --- not yet supported */
      break;

    case 4:
      /* access watchpoint */
      ob[ 0 ] = 0; /* FIXME --- not yet supported */
      break;
    }
  }
}


/**
 * Remove Break or Watch Point.
 *
 * \param ib buffer with string received from GDB
 * \param ob buffer to store string to be sent to GDB
 */
void AC_GDB::break_remove( char *ib, char *ob ) {
  int type, address, length;

  if ( sscanf( ib, "z%x,%x,%x", &type, &address, &length ) != 3 )
    /* Data is wrong! */
    strncpy( ob, "E01", GDB_BUFFERSIZE );
  else {
    switch ( type )
      {
      case 0:
	/* memory breakpoint */
	if ( bps->remove( address ) == 0 )
	  strncpy( ob, "OK", GDB_BUFFERSIZE );
	else
	  strncpy( ob, "E00", GDB_BUFFERSIZE );
	break;

      case 1:
	/* hardware breakpoint */
	ob[ 0 ] = 0; /* FIXME --- not yet supported */
	break;

      case 2:
	/* write watchpoint */
	ob[ 0 ] = 0; /* FIXME --- not yet supported */
	break;

      case 3:
	/* read watchpoint */
	ob[ 0 ] = 0; /* FIXME --- not yet supported */
	break;

      case 4:
	/* access watchpoint */
	ob[ 0 ] = 0; /* FIXME --- not yet supported */
	break;
      }
  }
}





/* GDB Specific Functions: ***************************************************/

/**
 * Establish TCP Connection
 */
void AC_GDB::connect() {
  struct sockaddr_in cliAddr, servAddr;
  int sd;

  /* Create Socket */
  if ( (sd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("Cannot Open Socket");
    ::exit(127);
  }

  /* Bind To Server Port */
  servAddr.sin_family = AF_INET;
  servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servAddr.sin_port = htons(this->port);

  { /* reuse address */
    int yes=1;
    if ( setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
      perror( "setsockopt" );
      ::exit(127);
    }
  }

  fprintf(stderr, "AC_GDB: listening to port %d\n", this->port);
  if(bind(sd, (struct sockaddr *) &servAddr, sizeof(servAddr))<0) {
    perror("bind");
    ::exit(127);
  }

  listen(sd,5);

  /* Listen for a connection */
  {
    socklen_t cliLen;
    cliLen = sizeof(cliAddr);

    if ((this->sd = accept(sd, (struct sockaddr *) &cliAddr, &cliLen)) < 0 ) {
      perror("accept");
      ::exit(127);
    }
  }

  connected = 1;
  fprintf(stderr, "AC_GDB: connected to port %d\n", this->port);
}


/*******************************************************************************
 * Process break point.                                                        *
 ******************************************************************************/

/**
 *    Return if the processor must stop or not. It must stop if it's the first 
 * time, it's in step mode or there's a breakpoint for that address.
 *
 * \param decoded_pc decoded program counter (PC, current address).
 *
 * \return true if it must stop, false otherwise.
 */
bool AC_GDB::stop(unsigned int decoded_pc) {
  if ( disabled ) return false;
  
  if ( first_time || step || bps->exists(decoded_pc))
    return true;
  return false;
}


/**
 * Process the next packet from gdb and take the needed action.
 */
void AC_GDB::process_bp() {
  if ( disabled ) return;
  first_time=0;
  
  snprintf( out_buffer, GDB_BUFFERSIZE, "S%02x", SIGTRAP );
  comm_putpacket(out_buffer);
  
  if ( ! connected ) return;

  while (1) {

    out_buffer[0] = 0;

    comm_getpacket(in_buffer);

    switch (in_buffer[0]) {
    case '?':
      /* "?": Return the reason simulator halted */
      snprintf( out_buffer, GDB_BUFFERSIZE, "S%02x", SIGTRAP );
      break;

    case 'g':
      /* "g": return the value of the CPU registers as hex data or ENN */
      reg_read( in_buffer, out_buffer );
      break;

    case 'G':
      /* "G": set the value of the CPU registers. */
      reg_write( in_buffer, out_buffer );
      break;

    case 'm':
      /* "mAA..AA,LLLL": Read LLLL bytes at address AA..AA as hex data or ENN*/
      mem_read( in_buffer, out_buffer );
      break;

    case 'M':
      /* "MAA..AA,LLLL:": Write LLLL bytes at address AA.AA return OK */
      mem_write( in_buffer, out_buffer );
      break;

    case 'c':
      /* "cAA..AA": continue at address AA..AA or same address if no AA..AA*/
      continue_execution( in_buffer, out_buffer );
      return;

    case 's':
      /* "sAA..AA": resume at address AA..AA or same address if no AA..AA */
      stepmode( in_buffer, out_buffer );
      return;

    case 0x03:
      /* Control-C: return control to gdb */
      cc( in_buffer, out_buffer );
      comm_putpacket( out_buffer );
      return;

    case 'k' :
      /* "k": kill */
      ac_stop();
      break;

    case 'Z':
      /* "ZT,AA..AA,LLLL": Insert breakpoint or watchpoint */
      break_insert( in_buffer, out_buffer );
      break;

    case 'z':
      /* "zT,AA..AA,LLLL": Remove breakpoint or watchpoint */
      break_remove( in_buffer, out_buffer );
      break;

    default:
      out_buffer[0] = 0; /* Null response */
    }

    comm_putpacket(out_buffer);
  }
}









/* Helper Functions: *********************************************************/

/* Copyright notice:
 *
 *		Most of this code was taken from: arch/mips/kernel/gdb-stub.c
 *
 * Original copyright:
 *
 *	Originally written by Glenn Engel, Lake Stevens Instrument Division
 *
 *	Contributed by HP Systems
 *
 *	Modified for SPARC by Stu Grossman, Cygnus Support.
 *
 *	Modified for Linux/MIPS (and MIPS in general) by Andreas Busse
 *	Send complaints, suggestions etc. to <andy@waldorf-gmbh.de>
 *
 *	Copyright (C) 1995 Andreas Busse
 *
 *	Copyright (C) 2003 MontaVista Software Inc.
 *	Author: Jun Sun, jsun@mvista.com or jsun@junsun.net
 */

/**
 *	Convert from a hex digit to an int
 *
 * \param ch hexadecimal digit
 *
 * \return corresponding integer
 */
int AC_GDB::hex(unsigned char ch)
{
  if (ch >= 'a' && ch <= 'f')
    return ch-'a'+10;
  if (ch >= '0' && ch <= '9')
    return ch-'0';
  if (ch >= 'A' && ch <= 'F')
    return ch-'A'+10;
  return -1;
}



/**
 * Change word endianess.
 *
 * \param src source word
 *
 * \return endianess swapped word
 */
ac_word AC_GDB::changeendianess( ac_word src )
{
  ac_word dst = 0;
  int i, j;
  int size = sizeof( src );
  
  for ( i=0, j=( size - 1 );
	i < size;
	i++, j-- )
    dst |= ( ( src >> ( i << 3 ) ) & 0xff ) << ( j << 3 );
  
  return dst;
}


/**
 * scan for the sequence $<data>#<checksum>
 *
 * \param buffer buffer to receive the packet.
 */
void AC_GDB::comm_getpacket (char *buffer) {
  unsigned char checksum;
  unsigned char xmitcsum;
  int i;
  int count;
  unsigned char ch;

  do {
    /*
     * wait around for the start character,
     * ignore all other characters
     */
    while ( ( ch = ( comm_getchar() & 0x7f ) ) != '$' );

    checksum = 0;
    xmitcsum = 255;
    count = 0;

    /*
     * now, read until a # or end of buffer is found
     */
    while ( count < GDB_BUFFERSIZE )
      {
	ch = comm_getchar();
	if ( ch == '#' )
	  break;
	checksum = checksum + ch;
	buffer[ count ] = ch;
	count = count + 1;
      }

    if ( count >= GDB_BUFFERSIZE )
      continue;

    buffer[ count ] = 0;

    if ( ch == '#' )
      {
	xmitcsum	= hex( comm_getchar() & 0x7f ) << 4;
	xmitcsum |= hex( comm_getchar() & 0x7f );

	if ( checksum != xmitcsum )
	  comm_putchar( '-' ); /* failed checksum */
	else {
	  comm_putchar( '+' ); /* successful transfer */

	  /*
	   * if a sequence char is present,
	   * reply the sequence ID
	   */
	  if ( buffer[ 2 ] == ':' )
	    {
	      comm_putchar( buffer[0] );
	      comm_putchar( buffer[1] );

	      /*
	       * remove sequence chars from buffer
	       */
	      count = strlen( buffer );
	      for ( i = 3; i <= count; i ++ )
		buffer[ i - 3] = buffer[ i ];
	    }
	}
      }
  }
  while ( checksum != xmitcsum );

  debug("received packet:" << buffer);
}



/**
 * send the packet in buffer.
 *
 * \param buffer string to be sent.
 */
void AC_GDB::comm_putpacket(const char *buffer) {
  unsigned char checksum;
  int count;
  unsigned char ch;

  debug("out packet:" << buffer << endl);

  /*
   * $<packet info>#<checksum>.
   */

  do
    {
      comm_putchar( '$' );
      checksum = 0;
      count		 = 0;

      while ( ( ch = buffer[ count ] ) != 0 )
	{
	  if ( ! ( comm_putchar( ch ) ) )
	    return;
	  checksum += ch;
	  count		 += 1;
	}

      comm_putchar( '#' );
      comm_putchar( hexchars[ checksum >> 4 ] );
      comm_putchar( hexchars[ checksum & 0xf ] );

    }
  while ( ( comm_getchar() & 0x7f) != '+' );
}


/**
 * Put char (byte) to output queue.
 *
 * \param c char to be sent.
 */
int AC_GDB::comm_putchar(const char c) {
  return write( sd, &c, 1 );
}


/**
 * Get char (byte) from input queue.
 *
 * \return char from input queue.
 */
char AC_GDB::comm_getchar() {
  char c;
  read( sd, &c, 1 );
  return(c);
}



/**
 * Disable GDB support
 */
void AC_GDB::disable() {
  this->disabled = 1;
}

/**
 * Enable GDB support
 */
void AC_GDB::enable() {
  this->disabled = 0;
}



/**
 * Return if GDB Support is disabled or not
 *
 * \return 1 if it's disabled, 0 otherwise;
 */
int AC_GDB::is_disabled() {
  return (int) this->disabled;
}



/**
 * Set GDB port. Just have effect on next AC_GDB::connect().
 *
 * \param port new port
 */
void AC_GDB::set_port( int port ) {
  this->port = port;
}

/**
 * Get GDB port.
 *
 * \return current gdb port
 */
int AC_GDB::get_port() {
  return this->port;
}
