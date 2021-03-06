/** Implementation of the pqxx::result class and support classes.
 *
 * pqxx::result represents the set of result rows from a database query.
 *
 * Copyright (c) 2000-2020, Jeroen T. Vermeulen.
 *
 * See COPYING for copyright license.  If you did not receive a file called
 * COPYING with this source code, please notify the distributor of this
 * mistake, or contact the author.
 */
#include "pqxx-source.hxx"

#include <cstdlib>
#include <cstring>

extern "C"
{
#include <libpq-fe.h>
}

#include "pqxx/except"
#include "pqxx/result"


pqxx::row::row(result const &r, result::size_type i) noexcept :
        m_result{r},
        m_index{i},
        m_end{r.columns()}
{}


pqxx::row::const_iterator pqxx::row::begin() const noexcept
{
  return const_iterator{*this, m_begin};
}


pqxx::row::const_iterator pqxx::row::cbegin() const noexcept
{
  return begin();
}


pqxx::row::const_iterator pqxx::row::end() const noexcept
{
  return const_iterator{*this, m_end};
}


pqxx::row::const_iterator pqxx::row::cend() const noexcept
{
  return end();
}


pqxx::row::reference pqxx::row::front() const noexcept
{
  return field{*this, m_begin};
}


pqxx::row::reference pqxx::row::back() const noexcept
{
  return field{*this, m_end - 1};
}


pqxx::row::const_reverse_iterator pqxx::row::rbegin() const
{
  return const_reverse_row_iterator{end()};
}


pqxx::row::const_reverse_iterator pqxx::row::crbegin() const
{
  return rbegin();
}


pqxx::row::const_reverse_iterator pqxx::row::rend() const
{
  return const_reverse_row_iterator{begin()};
}


pqxx::row::const_reverse_iterator pqxx::row::crend() const
{
  return rend();
}


bool pqxx::row::operator==(row const &rhs) const noexcept
{
  if (&rhs == this)
    return true;
  auto const s{size()};
  if (rhs.size() != s)
    return false;
  for (size_type i{0}; i < s; ++i)
    if ((*this)[i] != rhs[i])
      return false;
  return true;
}


pqxx::row::reference pqxx::row::operator[](size_type i) const noexcept
{
  return field{*this, m_begin + i};
}


pqxx::row::reference pqxx::row::operator[](char const f[]) const
{
  return at(f);
}


void pqxx::row::swap(row &rhs) noexcept
{
  auto const i{m_index};
  auto const b{m_begin};
  auto const e{m_end};
  m_result.swap(rhs.m_result);
  m_index = rhs.m_index;
  m_begin = rhs.m_begin;
  m_end = rhs.m_end;
  rhs.m_index = i;
  rhs.m_begin = b;
  rhs.m_end = e;
}


pqxx::field pqxx::row::at(char const f[]) const
{
  return field{*this, m_begin + column_number(f)};
}


pqxx::field pqxx::row::at(pqxx::row::size_type i) const
{
  if (i >= size())
    throw range_error{"Invalid field number."};

  return operator[](i);
}


pqxx::oid pqxx::row::column_type(size_type col_num) const
{
  return m_result.column_type(m_begin + col_num);
}


pqxx::oid pqxx::row::column_table(size_type col_num) const
{
  return m_result.column_table(m_begin + col_num);
}


pqxx::row::size_type pqxx::row::table_column(size_type col_num) const
{
  return m_result.table_column(m_begin + col_num);
}


pqxx::row::size_type pqxx::row::column_number(char const col_name[]) const
{
  auto const n{m_result.column_number(col_name)};
  if (n >= m_end)
    throw argument_error{"Column '" + std::string{col_name} +
                         "' falls outside slice."};
  if (n >= m_begin)
    return n - m_begin;

  // XXX: Why did we do this?
  char const *const adapted_name{m_result.column_name(n)};
  for (auto i{m_begin}; i < m_end; ++i)
    if (strcmp(adapted_name, m_result.column_name(i)) == 0)
      return i - m_begin;

  return result{}.column_number(col_name);
}


pqxx::row pqxx::row::slice(size_type sbegin, size_type send) const
{
  if (sbegin > send or send > size())
    throw range_error{"Invalid field range."};

  row res{*this};
  res.m_begin = m_begin + sbegin;
  res.m_end = m_begin + send;
  return res;
}


bool pqxx::row::empty() const noexcept
{
  return m_begin == m_end;
}


pqxx::const_row_iterator pqxx::const_row_iterator::operator++(int)
{
  auto const old{*this};
  m_col++;
  return old;
}


pqxx::const_row_iterator pqxx::const_row_iterator::operator--(int)
{
  auto const old{*this};
  m_col--;
  return old;
}


pqxx::const_row_iterator pqxx::const_reverse_row_iterator::base() const
  noexcept
{
  iterator_type tmp{*this};
  return ++tmp;
}


pqxx::const_reverse_row_iterator pqxx::const_reverse_row_iterator::
operator++(int)
{
  auto tmp{*this};
  operator++();
  return tmp;
}


pqxx::const_reverse_row_iterator pqxx::const_reverse_row_iterator::
operator--(int)
{
  auto tmp{*this};
  operator--();
  return tmp;
}
