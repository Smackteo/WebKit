# Generated by Django 3.2.25 on 2025-07-10 15:40

from django.db import migrations


class Migration(migrations.Migration):

    dependencies = [
        ('ews', '0009_alter_change_comment_id'),
    ]

    operations = [
        migrations.RenameField(
            model_name='change',
            old_name='pr_id',
            new_name='pr_number',
        ),
    ]
