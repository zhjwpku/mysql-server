/* Copyright (c) 2018, 2019, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/basic_ostream.h"
#include "my_inttypes.h"
#include "mysql/components/services/log_shared.h"
#include "mysql/psi/mysql_file.h"
#include "mysqld_error.h"
#include "sql/log.h"

IO_CACHE_ostream::IO_CACHE_ostream() {}
IO_CACHE_ostream::~IO_CACHE_ostream() { close(); }

bool IO_CACHE_ostream::open(
#ifdef HAVE_PSI_INTERFACE
    PSI_file_key log_file_key MY_ATTRIBUTE((unused)),
#endif
    const char *file_name, myf flags) {
  File file = -1;

  if ((file = mysql_file_open(log_file_key, file_name, O_CREAT | O_WRONLY,
                              MYF(MY_WME))) < 0)
    return true;

  if (init_io_cache(&m_io_cache, file, IO_SIZE, WRITE_CACHE, 0, false, flags)) {
    mysql_file_close(file, MYF(0));
    return true;
  }
  return false;
}

bool IO_CACHE_ostream::close() {
  if (my_b_inited(&m_io_cache)) {
    int ret = end_io_cache(&m_io_cache);
    ret |= mysql_file_close(m_io_cache.file, MYF(MY_WME));
    return ret != 0;
  }
  return false;
}

bool IO_CACHE_ostream::seek(my_off_t offset) {
  DBUG_ASSERT(my_b_inited(&m_io_cache));
  return reinit_io_cache(&m_io_cache, WRITE_CACHE, offset, false, true);
}

bool IO_CACHE_ostream::write(const unsigned char *buffer, my_off_t length) {
  DBUG_ASSERT(my_b_inited(&m_io_cache));
  DBUG_EXECUTE_IF("simulate_ostream_write_failure", return true;);
  return my_b_safe_write(&m_io_cache, buffer, length);
}

bool IO_CACHE_ostream::truncate(my_off_t offset) {
  DBUG_ASSERT(my_b_inited(&m_io_cache));
  DBUG_ASSERT(m_io_cache.file != -1);

  if (my_chsize(m_io_cache.file, offset, 0, MYF(MY_WME))) return true;

  reinit_io_cache(&m_io_cache, WRITE_CACHE, offset, false, true);
  return false;
}

bool IO_CACHE_ostream::flush() {
  DBUG_ASSERT(my_b_inited(&m_io_cache));
  return flush_io_cache(&m_io_cache);
}

bool IO_CACHE_ostream::sync() {
  DBUG_ASSERT(my_b_inited(&m_io_cache));
  return mysql_file_sync(m_io_cache.file, MYF(MY_WME)) != 0;
}

MMAP_ostream::MMAP_ostream() {}
MMAP_ostream::~MMAP_ostream() { close(); }

bool MMAP_ostream::open(PSI_file_key log_file_key, const char *file_name,
                        ulong mmap_length) {
  File file{-1};
  if ((file = mysql_file_open(log_file_key, file_name, O_CREAT | O_RDWR,
                              MYF(MY_WME))) < 0) {
    return true;
  }

  if (my_chsize(file, mmap_length, 0, MYF(MY_WME))) {
    mysql_file_close(file, MYF(0));
    return true;
  }

  if (init_mmap_info(&m_mmap_info, file, mmap_length, 0)) {
    mysql_file_close(file, MYF(0));
    return true;
  }

  return false;
}

bool MMAP_ostream::close() {
  int ret = end_mmap_info(&m_mmap_info);
  DBUG_ASSERT(m_mmap_info.file > 0);
  DBUG_ASSERT(m_mmap_info.sync_pos == m_mmap_info.write_pos);
  DBUG_ASSERT(m_mmap_info.end_pos_of_file >=
              static_cast<my_off_t>(m_mmap_info.write_pos - m_mmap_info.addr));
  ret |= my_chsize(m_mmap_info.file,
                   static_cast<size_t>(m_mmap_info.end_pos_of_file), 0,
                   MYF(MY_WME));
  ret |= mysql_file_close(m_mmap_info.file, MYF(MY_WME));
  return ret != 0;
}

bool MMAP_ostream::seek(my_off_t offset) {
  DBUG_ASSERT(offset < m_mmap_info.mmap_length);
  m_mmap_info.write_pos = m_mmap_info.addr + offset;
  m_mmap_info.sync_pos = m_mmap_info.write_pos;
  if (m_mmap_info.end_pos_of_file < offset) {
    m_mmap_info.end_pos_of_file = offset;
  }

  return false;
}

bool MMAP_ostream::write(const unsigned char *buffer, my_off_t length) {
  DBUG_ASSERT(m_mmap_info.write_pos + length < m_mmap_info.mmap_end);
  memcpy(m_mmap_info.write_pos, buffer, length);
  m_mmap_info.write_pos += length;
  my_off_t offset =
      static_cast<my_off_t>(m_mmap_info.write_pos - m_mmap_info.addr);
  if (offset > m_mmap_info.end_pos_of_file) {
    m_mmap_info.end_pos_of_file = offset;
  }

  return false;
}

bool MMAP_ostream::truncate(my_off_t offset) {
  if (my_chsize(m_mmap_info.file, offset, 0, MYF(MY_WME))) {
    return true;
  }
  if (unlikely(offset < m_mmap_info.end_pos_of_file)) {
    m_mmap_info.end_pos_of_file = offset;
  }

  // See IO_CACHE_ostream::truncate again to check truncate behaviour
  if (m_mmap_info.write_pos > m_mmap_info.addr + offset) {
    m_mmap_info.write_pos = m_mmap_info.addr + offset;
    m_mmap_info.sync_pos = m_mmap_info.write_pos;
  }

  return false;
}

bool MMAP_ostream::sync() {
  my_msync(m_mmap_info.file, m_mmap_info.sync_pos,
           static_cast<size_t>(m_mmap_info.write_pos - m_mmap_info.sync_pos),
           MS_SYNC);
  m_mmap_info.sync_pos = m_mmap_info.write_pos;

  return false;
}

Compressed_ostream::Compressed_ostream() : m_compressor(nullptr) {}

Compressed_ostream::~Compressed_ostream() {}

binary_log::transaction::compression::Compressor *
Compressed_ostream::get_compressor() {
  return m_compressor;
}

void Compressed_ostream::set_compressor(
    binary_log::transaction::compression::Compressor *c) {
  m_compressor = c;
}

bool Compressed_ostream::write(const unsigned char *buffer, my_off_t length) {
  if (m_compressor == nullptr) return true;
  auto res{false};
  auto left{0};
  std::tie(left, res) = m_compressor->compress(buffer, length);
  return (res || left > 0);
}
